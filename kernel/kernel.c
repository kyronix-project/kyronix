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
#include "drivers/video/fb.h"
#include "proc/loadavg.h"
#include "proc/ns.h"
#include "proc/smp.h"
#include "version.h"

#include "crypto/chacha20.h"
#include "drivers/acpi/acpi.h"
#include "drivers/ata/ahci.h"
#include "drivers/block/block.h"
#include "drivers/block/blockdev.h"
#include "drivers/bus/pci/pci.h"
#include "drivers/bus/spi/spi.h"
#include "drivers/char/serial.h"
#include "drivers/char/uio.h"
#include "drivers/hwmon/tmp117.h"
#include "drivers/input/input.h"
#include "drivers/input/kbd.h"
#include "drivers/input/ps2mouse.h"
#include "drivers/tty/tty.h"
#include "drivers/tty/vt.h"
#include "drivers/usb/usb.h"
#include "drivers/video/fbdev.h"
#include "exec/process.h"
#include "fs/cpio.h"
#include "fs/ext2.h"
#include "fs/fstab.h"
#include "fs/partition.h"
#include "fs/squashfs.h"
#include "fs/vfs.h"
#include "fs/vfs_internal.h"
#include "lib/inflate.h"
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

uint8_t boot_id[16];

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
    ns_init();
    kstatus("Initialising scheduler", true);
    loadavg_init();
    kstatus("Initialising load average", true);
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
    spi_stub_create_controller(0);
    tmp117_subsys_init();
    kstatus("Initialising TMP117 sensor", true);
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
        chacha20_rng_bytes(&g_chacha20_rng, boot_id, sizeof(boot_id));
        boot_id[6] = (uint8_t) (0x40 | (boot_id[6] & 0x0F));
        boot_id[8] = (uint8_t) (0x80 | (boot_id[8] & 0x3F));
        kstatus("Initialising CSPRNG", true);
    }

    sti();
    ps2mouse_init();
    kstatus("Initialising PS/2 mouse", true);
    usb_init();
    kstatus("Initialising USB", true);
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

    {
        static const uint8_t inf_dyn_in[] = {
			0x6b, 0x79, 0x72, 0x6f, 0x6e, 0x69, 0x78, 0x20, 0x73, 0x71, 0x75, 0x61, 0x73, 0x68,
			0x66, 0x73, 0x20, 0x69, 0x6e, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x20, 0x73, 0x65, 0x6c,
			0x66, 0x74, 0x65, 0x73, 0x74, 0x20, 0x70, 0x61, 0x79, 0x6c, 0x6f, 0x61, 0x64, 0x0a,
			0x6b, 0x79, 0x72, 0x6f, 0x6e, 0x69, 0x78, 0x20, 0x73, 0x71, 0x75, 0x61, 0x73, 0x68,
			0x66, 0x73, 0x20, 0x69, 0x6e, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x20, 0x73, 0x65, 0x6c,
			0x66, 0x74, 0x65, 0x73, 0x74, 0x20, 0x70, 0x61, 0x79, 0x6c, 0x6f, 0x61, 0x64, 0x0a,
			0x6b, 0x79, 0x72, 0x6f, 0x6e, 0x69, 0x78, 0x20, 0x73, 0x71, 0x75, 0x61, 0x73, 0x68,
			0x66, 0x73, 0x20, 0x69, 0x6e, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x20, 0x73, 0x65, 0x6c,
			0x66, 0x74, 0x65, 0x73, 0x74, 0x20, 0x70, 0x61, 0x79, 0x6c, 0x6f, 0x61, 0x64, 0x0a,
			0x6b, 0x79, 0x72, 0x6f, 0x6e, 0x69, 0x78, 0x20, 0x73, 0x71, 0x75, 0x61, 0x73, 0x68,
			0x66, 0x73, 0x20, 0x69, 0x6e, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x20, 0x73, 0x65, 0x6c,
			0x66, 0x74, 0x65, 0x73, 0x74, 0x20, 0x70, 0x61, 0x79, 0x6c, 0x6f, 0x61, 0x64, 0x0a,
			0x6b, 0x79, 0x72, 0x6f, 0x6e, 0x69, 0x78, 0x20, 0x73, 0x71, 0x75, 0x61, 0x73, 0x68,
			0x66, 0x73, 0x20, 0x69, 0x6e, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x20, 0x73, 0x65, 0x6c,
			0x66, 0x74, 0x65, 0x73, 0x74, 0x20, 0x70, 0x61, 0x79, 0x6c, 0x6f, 0x61, 0x64, 0x0a,
			0x6b, 0x79, 0x72, 0x6f, 0x6e, 0x69, 0x78, 0x20, 0x73, 0x71, 0x75, 0x61, 0x73, 0x68,
			0x66, 0x73, 0x20, 0x69, 0x6e, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x20, 0x73, 0x65, 0x6c,
			0x66, 0x74, 0x65, 0x73, 0x74, 0x20, 0x70, 0x61, 0x79, 0x6c, 0x6f, 0x61, 0x64, 0x0a,
        };
        static const uint8_t inf_dyn_comp[] = {
            0xcb, 0xae, 0x2c, 0xca, 0xcf, 0xcb, 0xac, 0x50, 0x28, 0x2e, 0x2c, 0x4d, 0x2c, 0xce,
            0x48, 0x2b, 0x56, 0xc8, 0xcc, 0x4b, 0xcb, 0x49, 0x2c, 0x49, 0x55, 0x28, 0x4e, 0xcd,
            0x49, 0x2b, 0x49, 0x2d, 0x2e, 0x51, 0x28, 0x48, 0xac, 0xcc, 0xc9, 0x4f, 0x4c, 0xe1,
            0xca, 0x1e, 0x76, 0x2a, 0x01,
        };
        static const uint8_t inf_fix_in[] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
            0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b,
            0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
            0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
            0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x72, 0x65, 0x70, 0x65, 0x61, 0x74,
            0x20, 0x72, 0x65, 0x70, 0x65, 0x61, 0x74, 0x20, 0x72, 0x65, 0x70, 0x65, 0x61, 0x74,
            0x0a,
        };
        static const uint8_t inf_fix_comp[] = {
            0x63, 0x60, 0x64, 0x62, 0x66, 0x61, 0x65, 0x63, 0xe7, 0xe0, 0xe4, 0xe2, 0xe6, 0xe1,
            0xe5, 0xe3, 0x17, 0x10, 0x14, 0x12, 0x16, 0x11, 0x15, 0x13, 0x97, 0x90, 0x94, 0x92,
            0x96, 0x91, 0x95, 0x93, 0x57, 0x50, 0x54, 0x52, 0x56, 0x51, 0x55, 0x53, 0xd7, 0xd0,
            0xd4, 0xd2, 0xd6, 0xd1, 0xd5, 0xd3, 0x37, 0x30, 0x34, 0x32, 0x36, 0x31, 0x35, 0x33,
            0xb7, 0xb0, 0xb4, 0xb2, 0xb6, 0xb1, 0xb5, 0xb3, 0x2f, 0x4a, 0x2d, 0x48, 0x4d, 0x2c,
            0x51, 0x40, 0xa1, 0xb8, 0x00,
        };
        static const uint8_t inf_sto_in[] = {
            0x73, 0x74, 0x6f, 0x72, 0x65, 0x64, 0x20, 0x62, 0x6c, 0x6f, 0x63, 0x6b, 0x20, 0x61,
            0x6c, 0x69, 0x67, 0x6e, 0x6d, 0x65, 0x6e, 0x74, 0x20, 0x63, 0x68, 0x65, 0x63, 0x6b,
            0x3a, 0x20, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x41, 0x42,
            0x43, 0x44, 0x45, 0x46, 0x0a,
        };
        static const uint8_t inf_sto_comp[] = {
            0x01, 0x2f, 0x00, 0xd0, 0xff, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x64, 0x20, 0x62, 0x6c,
            0x6f, 0x63, 0x6b, 0x20, 0x61, 0x6c, 0x69, 0x67, 0x6e, 0x6d, 0x65, 0x6e, 0x74, 0x20,
            0x63, 0x68, 0x65, 0x63, 0x6b, 0x3a, 0x20, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x38, 0x39, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x0a,
        };

        uint8_t out[512];
        bool inflate_ok = true;

        int64_t n = kinflate(inf_dyn_comp, sizeof(inf_dyn_comp), out, sizeof(out));
        if (n != (int64_t) sizeof(inf_dyn_in) ||
            memcmp(out, inf_dyn_in, sizeof(inf_dyn_in)) != 0)
            inflate_ok = false;
        n = kinflate(inf_fix_comp, sizeof(inf_fix_comp), out, sizeof(out));
        if (n != (int64_t) sizeof(inf_fix_in) ||
            memcmp(out, inf_fix_in, sizeof(inf_fix_in)) != 0)
            inflate_ok = false;
        n = kinflate(inf_sto_comp, sizeof(inf_sto_comp), out, sizeof(out));
        if (n != (int64_t) sizeof(inf_sto_in) ||
            memcmp(out, inf_sto_in, sizeof(inf_sto_in)) != 0)
            inflate_ok = false;
        n = kinflate(inf_dyn_comp, sizeof(inf_dyn_comp) - 3u, out, sizeof(out));
        if (n >= 0) inflate_ok = false;
        n = kinflate(inf_dyn_comp, sizeof(inf_dyn_comp), out, 8u);
        if (n >= 0) inflate_ok = false;

        log_info("inflate test: %s", inflate_ok ? "PASS" : "FAIL");
    }

    // run_mm_benchmarks();

    if (hhdm_req.response) log_info("hhdm offset : 0x%016lx", hhdm_req.response->offset);

    log_info("fb : %lux%lu  %u bpp", lfb->width, lfb->height, (unsigned) lfb->bpp);

    print_memmap();

    ext2_init();
    squashfs_init();

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

    {
        vfs_node_t *fstab_node = vfs_lookup("/etc/fstab");
        bool have_fstab = fstab_node != NULL;
        if (fstab_node) vfs_node_unref_internal(fstab_node);
        if (root_on_disk || have_fstab) {
            bool fstab_ok = fstab_mount_all("/etc/fstab");
            kstatus("Mounting fstab entries", fstab_ok);
        }
    }

    {
        int result = module_load_path("/lib/modules/virtio_net.ko");
        if (result == 0) { kstatus("Loading virtio-net module", true); }
    }
    {
        int result = module_load_path("/lib/modules/e1000.ko");
        if (result == 0) { kstatus("Loading e1000 module", true); }
    }
    {
        int result = module_load_path("/lib/modules/rtl8168.ko");
        if (result == 0) { kstatus("Loading RTL8168 module", true); }
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
