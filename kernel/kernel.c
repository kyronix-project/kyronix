#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/pit.h"
#include "arch/x86_64/syscall_setup.h"
#include "boot/limine.h"
#include "drivers/fb.h"
#include "proc/smp.h"
#include "version.h"

#include "crypto/chacha20.h"
#include "drivers/acpi.h"
#include "drivers/ahci.h"
#include "drivers/block.h"
#include "drivers/blockdev.h"
#include "drivers/fbdev.h"
#include "drivers/input.h"
#include "drivers/kbd.h"
#include "drivers/pci.h"
#include "drivers/ps2mouse.h"
#include "drivers/serial.h"
#include "drivers/tty.h"
#include "drivers/uio.h"
#include "drivers/vt.h"
#include "exec/process.h"
#include "fs/cpio.h"
#include "fs/ext2.h"
#include "fs/fstab.h"
#include "fs/partition.h"
#include "fs/vfs.h"
#include "fs/vfs_internal.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "module/loader.h"
#include "proc/jail.h"
#include "proc/proc.h"
#include "security/anti_toctou.h"
#include "security/phantom.h"

#define STATUS_COL 72
#define COL_GRN "\033[0;32m"
#define COL_RED "\033[0;31m"
#define COL_BOLD "\033[1m"
#define COL_RST "\033[0m"

LIMINE_REQUESTS_START_MARKER;
LIMINE_BASE_REVISION(3);

static volatile struct limine_framebuffer_request fb_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = NULL,
};

static volatile struct limine_memmap_request mmap_req = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = NULL,
};

static volatile struct limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = NULL,
};

static volatile struct limine_module_request mod_req = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
    .response = NULL,
    .internal_module_count = 0,
    .internal_modules = NULL,
};

static volatile struct limine_rsdp_request rsdp_req = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0,
    .response = NULL,
};

static volatile struct limine_kernel_address_request kaddr_req = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
    .response = NULL,
};

LIMINE_REQUESTS_END_MARKER;

static void kernel_putchar(char c, void *ctx) {
    (void) ctx;
    tty_putchar(c);
}

static void kstatus(const char *msg, bool ok) {
    kprintf(COL_GRN " *" COL_RST " %s ...\033[%dG[ %s%s" COL_RST " ]\n", msg, STATUS_COL,
            ok ? COL_GRN : COL_RED, ok ? "ok" : "!!");
}

static const char *memmap_type_name(uint64_t type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE:
        return "Usable";
    case LIMINE_MEMMAP_RESERVED:
        return "Reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
        return "ACPI reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS:
        return "ACPI NVS";
    case LIMINE_MEMMAP_BAD_MEMORY:
        return "Bad memory";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
        return "Bootloader reclaimable";
    case LIMINE_MEMMAP_KERNEL_AND_MODULES:
        return "Kernel & modules";
    case LIMINE_MEMMAP_FRAMEBUFFER:
        return "Framebuffer";
    default:
        return "Unknown";
    }
}

static void print_memmap(void) {
    if (!mmap_req.response) {
        log_warn("no memory map");
        return;
    }
    struct limine_memmap_response *resp = mmap_req.response;
    uint64_t total_usable = 0;

    log_info("Memory map (%lu entries):", resp->entry_count);

    for (uint64_t i = 0; i < resp->entry_count; i++) {
        struct limine_memmap_entry *e = resp->entries[i];
        log_info("  %016lx-%016lx  %s", e->base, e->base + e->length, memmap_type_name(e->type));
        if (e->type == LIMINE_MEMMAP_USABLE) total_usable += e->length;
    }
    log_info("  Usable: %lu MiB", total_usable / (1024 * 1024));
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t) hi << 32) | lo;
}

#define BENCH_ITERS 100000

static void bench_pmm(void) {
    void **buf = (void **) kmalloc(BENCH_ITERS * sizeof(void *));
    if (!buf) {
        log_info("bench pmm  : SKIP (no mem for buf)");
        return;
    }

    uint64_t t0 = rdtsc();
    for (int i = 0; i < BENCH_ITERS; i++) buf[i] = pmm_alloc();
    uint64_t t1 = rdtsc();
    for (int i = 0; i < BENCH_ITERS; i++)
        if (buf[i]) pmm_free(buf[i]);
    uint64_t t2 = rdtsc();

    uint64_t dt_alloc = t1 - t0;
    uint64_t dt_free = t2 - t1;
    uint64_t alloc_mops = dt_alloc ? (uint64_t) BENCH_ITERS * 1000000ULL / dt_alloc : 0;
    uint64_t free_mops = dt_free ? (uint64_t) BENCH_ITERS * 1000000ULL / dt_free : 0;

    kprintf("bench pmm  : alloc %lu ops in %lu cycles (%lu M/OPS), free %lu ops in %lu cycles (%lu "
            "M/OPS)\n",
            (uint64_t) BENCH_ITERS, dt_alloc, alloc_mops, (uint64_t) BENCH_ITERS, dt_free,
            free_mops);

    kfree(buf);
}

static void bench_vmm(void) {
    const uint64_t base = 0xffff900100000000ULL;
    uint64_t *phys = (uint64_t *) kmalloc(BENCH_ITERS * sizeof(uint64_t));
    if (!phys) {
        kprintf("bench vmm  : SKIP (no mem for buf)\n");
        return;
    }

    int count = 0;
    for (int i = 0; i < BENCH_ITERS; i++) {
        phys[i] = (uint64_t) pmm_alloc();
        if (!phys[i]) {
            kprintf("bench vmm  : SKIP (no phys pages)\n");
            for (int j = 0; j < i; j++) pmm_free((void *) phys[j]);
            kfree(phys);
            return;
        }
        count++;
    }

    uint64_t t0 = rdtsc();
    for (int i = 0; i < count; i++)
        vmm_map(&g_kernel_space, base + (uint64_t) i * PAGE_SIZE, phys[i], VMM_KDATA);
    uint64_t t1 = rdtsc();
    for (int i = 0; i < count; i++) vmm_unmap(&g_kernel_space, base + (uint64_t) i * PAGE_SIZE);
    uint64_t t2 = rdtsc();

    for (int i = 0; i < count; i++) pmm_free((void *) phys[i]);
    kfree(phys);

    uint64_t dt_map = t1 - t0;
    uint64_t dt_unmap = t2 - t1;
    uint64_t map_mops = dt_map ? (uint64_t) count * 1000000ULL / dt_map : 0;
    uint64_t unmap_mops = dt_unmap ? (uint64_t) count * 1000000ULL / dt_unmap : 0;

    kprintf("bench vmm  : map %d ops in %lu cycles (%lu M/OPS), unmap %d ops in %lu cycles (%lu "
            "M/OPS)\n",
            count, dt_map, map_mops, count, dt_unmap, unmap_mops);
}

static void bench_heap(void) {
    void **buf = (void **) kmalloc(BENCH_ITERS * sizeof(void *));
    if (!buf) {
        kprintf("bench heap : SKIP (no mem for buf)\n");
        return;
    }

    uint64_t t0 = rdtsc();
    for (int i = 0; i < BENCH_ITERS; i++) buf[i] = kmalloc(64);
    uint64_t t1 = rdtsc();
    for (int i = 0; i < BENCH_ITERS; i++) kfree(buf[i]);
    uint64_t t2 = rdtsc();

    uint64_t dt_alloc = t1 - t0;
    uint64_t dt_free = t2 - t1;
    uint64_t alloc_mops = dt_alloc ? (uint64_t) BENCH_ITERS * 1000000ULL / dt_alloc : 0;
    uint64_t free_mops = dt_free ? (uint64_t) BENCH_ITERS * 1000000ULL / dt_free : 0;

    kprintf("bench heap : alloc %lu ops in %lu cycles (%lu M/OPS), free %lu ops in %lu cycles (%lu "
            "M/OPS)\n",
            (uint64_t) BENCH_ITERS, dt_alloc, alloc_mops, (uint64_t) BENCH_ITERS, dt_free,
            free_mops);

    kfree(buf);
}

static void run_mm_benchmarks(void) {
    kprintf("memory manager benchmark (%d iterations):\n", BENCH_ITERS);
    bench_pmm();
    bench_vmm();
    bench_heap();
    kprintf("\n");
}

void kmain(void) {
    serial_init(COM1);
    printf_set_putchar(kernel_putchar, NULL);

    gdt_init();
    idt_init();
    kbd_init();

    if (!mmap_req.response || !hhdm_req.response) {
        kprintf("FATAL: no memory map or HHDM from bootloader\n");
        cpu_halt();
    }

    extern uint8_t __bss_end[];
    uint64_t kernel_end_phys = 0;
    if (kaddr_req.response) {
        uint64_t kphys = kaddr_req.response->physical_base;
        uint64_t kvirt = kaddr_req.response->virtual_base;
        kernel_end_phys = kphys + ((uint64_t) __bss_end - kvirt);
    }

    {
        extern cpu_local_t g_cpu_local[MAX_CPUS];
        g_cpu_local[0].cpu_id = 0;
        wrmsr(0xC0000101, (uint64_t) &g_cpu_local[0]);
        wrmsr(0xC0000102, (uint64_t) &g_cpu_local[0]);
    }

    pmm_init(mmap_req.response, hhdm_req.response->offset, kernel_end_phys);

    if (!fb_req.response || fb_req.response->framebuffer_count < 1) cpu_halt();

    struct limine_framebuffer *lfb = fb_req.response->framebuffers[0];
    fb_init(lfb);
    fb_clear(COLOR_BG);

    kprintf(COL_BOLD "k9" COL_RST " kernel is starting up " COL_GRN "Kyronix " KERNEL_VERSION
                     " (x86_64)" COL_RST "\n\n");

    kstatus("Initialising PMM", true);

    vmm_init();
    kstatus("Initialising VMM", true);

    lapic_init();
    kstatus("Initialising APIC", true);

    smp_init();
    {
        uint32_t eax = 7, ebx = 0, ecx = 0;
        __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx) : : "edx");
        if (ebx & (1u << 7)) /* SMEP advertised */
        {
            uint64_t cr4;
            __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
            cr4 |= (1UL << 20);
            __asm__ volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");
        }
    }
    kstatus("Enabling CPU protections", true);
    heap_init();
    kstatus("Initialising heap", true);
    syscall_init();
    kstatus("Initialising syscalls", true);
    proc_init();
    kstatus("Initialising scheduler", true);
    jail_init();
    kstatus("Initialising jails", true);
    phantom_init();
    kstatus("Initialising phantom hooks", true);
    vfs_init();
    {
        vfs_node_t *_n = vfs_lookup("/proc");
        kstatus("Mounting /proc", _n != NULL);
        vfs_node_unref_internal(_n);
    }
    {
        vfs_node_t *_n = vfs_lookup("/sys");
        kstatus("Mounting /sys", _n != NULL);
        vfs_node_unref_internal(_n);
    }
    {
        vfs_node_t *_n = vfs_lookup("/dev/pts");
        kstatus("Mounting /dev/pts", _n != NULL);
        vfs_node_unref_internal(_n);
    }
    pci_enumerate();
    kstatus("Enumerating PCI", true);
    /* Limine base revision >= 3 reports the RSDP as a physical address. */
    acpi_init(rsdp_req.response ? (uint64_t) rsdp_req.response->address : 0);
    kstatus("Initialising ACPI", acpi_available());
    block_init();
    ahci_init();
    kstatus("Initialising AHCI", ahci_ready());
    blockdev_init();
    partition_scan_all();
    blockdev_create_all();
    kstatus("Initialising block devices", true);
    uio_init();
    kstatus("Initialising UIO", true);
    fbdev_init();
    {
        vfs_node_t *_n = vfs_lookup("/dev/fb0");
        kstatus("Registering framebuffer", _n != NULL);
        vfs_node_unref_internal(_n);
    }

    input_init();
    {
        vfs_node_t *_n0 = vfs_lookup("/dev/input/event0");
        vfs_node_t *_n1 = vfs_lookup("/dev/input/event1");
        kstatus("Initialising evdev", _n0 != NULL && _n1 != NULL);
        vfs_node_unref_internal(_n0);
        vfs_node_unref_internal(_n1);
    }
    vt_init();
    kstatus("Initialising virtual tty", true);
    pit_init();
    kstatus("Starting PIT timer", true);
    lapic_calibrate_timer();

    {
        proc_t *bsp_idle = proc_create_idle(0, ap_sched_loop);
        if (bsp_idle) {
            g_cpu_local[0].idle = bsp_idle;
            g_cpu_local[0].current = bsp_idle;
        }
    }
    smp_boot_aps();
    kstatus("Starting phantom fault worker", phantom_worker_start());
    kstatus("Starting Anti-TOCTOU jitter", anti_toctou_init());

    {
        uint8_t seed[32];
        uint32_t lo, hi;
        int rdrand_ok;
        __asm__ volatile("cpuid" : "=c"(lo) : "a"(1) : "ebx", "edx", "memory");
        rdrand_ok = (int) ((lo >> 30) & 1);
        if (rdrand_ok) {
            for (int i = 0; i < 32; i += 8) {
                uint64_t v;
                __asm__ volatile("1: rdrand %0; jnc 1b" : "=r"(v)::"cc");
                __builtin_memcpy(seed + i, &v, 8);
            }
        } else {
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            uint64_t tsc = (uint64_t) hi << 32 | lo;
            uint64_t ticks = g_ticks;
            __builtin_memcpy(seed, &tsc, 8);
            __builtin_memcpy(seed + 8, &ticks, 8);
            for (int i = 16; i < 32; i++) seed[i] = (uint8_t) (tsc >> ((i % 8) * 8)) ^ 0xBB;
        }
        chacha20_rng_init(&g_chacha20_rng, seed);
        kstatus("Initialising CSPRNG", true);
    }

    sti();
    ps2mouse_init();
    kstatus("Initialising PS/2 mouse", true);
    kprintf("\n");

    {
        void *p[4];
        bool pmm_ok = true;
        for (int i = 0; i < 4; i++) {
            p[i] = pmm_alloc();
            if (!p[i]) {
                pmm_ok = false;
                break;
            }
            volatile uint64_t *v = phys_to_virt((uint64_t) p[i]);
            *v = 0xDEADBEEFCAFEBABEULL;
            if (*v != 0xDEADBEEFCAFEBABEULL) {
                pmm_ok = false;
                break;
            }
        }
        for (int i = 0; i < 4 && pmm_ok; i++)
            for (int j = i + 1; j < 4; j++)
                if (p[i] == p[j]) {
                    pmm_ok = false;
                    break;
                }

        for (int i = 0; i < 4; i++)
            if (p[i]) pmm_free(p[i]);

        log_info("pmm test : %s", pmm_ok ? "PASS" : "FAIL");
    }

    {
        const uint64_t test_virt = 0xffff900000001000ULL;
        uint64_t test_phys = (uint64_t) pmm_alloc();
        bool vmm_ok = false;

        if (test_phys) {
            int r = vmm_map(&g_kernel_space, test_virt, test_phys, VMM_KDATA);
            if (r == 0) {
                volatile uint64_t *p = (volatile uint64_t *) test_virt;
                *p = 0xC0FFEE00DEADC0DEULL;
                vmm_ok = (*p == 0xC0FFEE00DEADC0DEULL);
                vmm_unmap(&g_kernel_space, test_virt);
            }
            pmm_free((void *) test_phys);
        }

        log_info("vmm test : %s", vmm_ok ? "PASS" : "FAIL");
    }

    {
        bool heap_ok = true;

        uint8_t *a = kmalloc(64);
        uint8_t *b = kmalloc(128);
        uint8_t *c = kmalloc(256);

        heap_ok = a && b && c && (a != b) && (b != c) && (a != c);

        if (heap_ok) {
            memset(a, 0xAA, 64);
            memset(b, 0xBB, 128);
            memset(c, 0xCC, 256);
            for (int i = 0; i < 64 && heap_ok; i++)
                if (a[i] != 0xAA) heap_ok = false;
            for (int i = 0; i < 128 && heap_ok; i++)
                if (b[i] != 0xBB) heap_ok = false;
            for (int i = 0; i < 256 && heap_ok; i++)
                if (c[i] != 0xCC) heap_ok = false;
        }

        kfree(b);
        uint8_t *d = kmalloc(64);
        heap_ok = heap_ok && (d != NULL);
        if (d) memset(d, 0xDD, 64);

        uint8_t *a2 = krealloc(a, 512);
        heap_ok = heap_ok && (a2 != NULL);
        if (a2) {
            for (int i = 0; i < 64 && heap_ok; i++)
                if (a2[i] != 0xAA) heap_ok = false;
            kfree(a2);
        } else {
            kfree(a);
        }
        kfree(c);
        kfree(d);

        log_info("heap test: %s", heap_ok ? "PASS" : "FAIL");
        heap_stats();
    }

    // run_mm_benchmarks();

    if (hhdm_req.response) log_info("hhdm offset : 0x%016lx", hhdm_req.response->offset);

    log_info("fb : %lux%lu  %u bpp", lfb->width, lfb->height, (unsigned) lfb->bpp);

    print_memmap();

    ext2_init();

    bool root_on_disk = false;
    {
        struct block_device *root_dev = NULL;
        struct filesystem *root_fs = NULL;
        for (int i = 0; i < block_count() && !root_dev; i++) {
            struct block_device *bd = block_get(i);
            if (!bd) continue;
            for (int f = 0; f < vfs_fs_count(); f++) {
                struct filesystem *fs = vfs_get_fs(f);
                if (fs->check_root && fs->check_root(bd)) {
                    root_dev = bd;
                    root_fs = fs;
                    break;
                }
            }
        }
        if (root_dev && root_fs) {
            root_on_disk = root_fs->mount(root_dev, "/");
            kstatus("Mounting root from disk", root_on_disk);
        } else {
            struct block_device *bd = block_first();
            if (bd) {
                struct filesystem *fs = vfs_find_fs("ext2");
                bool ok = fs && fs->mount && fs->mount(bd, "/mnt");
                kstatus("Mounting ext2 at /mnt", ok);
            }
        }
    }

    if (!root_on_disk) {
        if (mod_req.response && mod_req.response->module_count > 0) {
            struct limine_file *initrd = mod_req.response->modules[0];
            log_info("initrd: %s  (%lu bytes)", initrd->path, initrd->size);
            if (cpio_load(initrd->address, initrd->size) < 0) {
                kstatus("Loading initrd", false);
                log_warn("initrd parse failed");
            } else {
                kstatus("Loading initrd", true);
            }
        } else {
            kstatus("Loading initrd", false);
            log_warn("no initrd module");
        }
    }

    if (root_on_disk) {
        bool fstab_ok = fstab_mount_all("/etc/fstab");
        kstatus("Mounting fstab entries", fstab_ok);
    }

    {
        int result = module_load_path("/lib/modules/virtio_net.ko");
        kstatus("Loading virtio-net module", result == 0);
        if (result < 0) log_warn("virtio-net module load failed: %d", result);
    }

    {
        vfs_node_t *init_node = vfs_lookup("/init");
        if (!init_node) init_node = vfs_lookup("/sbin/init");
        if (!init_node) init_node = vfs_lookup("/bin/init");

        if (init_node && init_node->type == VFS_TYPE_REG) {
            uint64_t fsize = init_node->size;
            uint8_t *buf = (uint8_t *) kmalloc(fsize + 1);
            if (buf) {
                if (init_node->fs_ops && init_node->fs_ops->read)
                    init_node->fs_ops->read(init_node, (char *) buf, 0, fsize);
                else if (init_node->data)
                    memcpy(buf, init_node->data, fsize);
                buf[fsize] = '\0';
                if (process_exec(buf, fsize, "/init") < 0) {
                    kprintf("  FATAL: process_exec failed\n");
                    kfree(buf);
                } else {
                    __atomic_store_n(&g_kernel_ready, 1, __ATOMIC_RELEASE);
                }
            } else {
                kprintf("  FATAL: out of memory for /init\n");
            }
        } else {
            kprintf("  FATAL: /init not found\n");
        }
        if (init_node) vfs_node_unref_internal(init_node);
    }

    vfs_sync_all();
    fb_set_color(COLOR_GRAY, COLOR_BG);
    kprintf("  Kernel halted.\n");
    cpu_halt();
}
