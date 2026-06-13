#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pit.h"
#include "arch/x86_64/syscall_setup.h"
#include "boot/limine.h"
#include "drivers/fb.h"

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
#include "fs/vfs.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/proc.h"

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

LIMINE_REQUESTS_END_MARKER;

static void kernel_putchar(char c, void* ctx)
{
    (void) ctx;
    tty_putchar(c);
}

static void kstatus(const char* msg, bool ok)
{
    kprintf(COL_GRN " *" COL_RST " %s ...\033[%dG[ %s%s" COL_RST " ]\n", msg, STATUS_COL,
            ok ? COL_GRN : COL_RED, ok ? "ok" : "!!");
}

static const char* memmap_type_name(uint64_t type)
{
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

static void print_memmap(void)
{
    if (!mmap_req.response) {
        log_warn("no memory map");
        return;
    }
    struct limine_memmap_response* resp = mmap_req.response;
    uint64_t total_usable = 0;

    log_info("Memory map (%lu entries):", resp->entry_count);

    for (uint64_t i = 0; i < resp->entry_count; i++) {
        struct limine_memmap_entry* e = resp->entries[i];
        log_info("  %016lx-%016lx  %s", e->base, e->base + e->length, memmap_type_name(e->type));
        if (e->type == LIMINE_MEMMAP_USABLE)
            total_usable += e->length;
    }
    log_info("  Usable: %lu MiB", total_usable / (1024 * 1024));
}

void kmain(void)
{
    serial_init(COM1);
    printf_set_putchar(kernel_putchar, NULL);

    gdt_init();
    idt_init();
    kbd_init();

    if (!mmap_req.response || !hhdm_req.response) {
        kprintf("FATAL: no memory map or HHDM from bootloader\n");
        cpu_halt();
    }
    pmm_init(mmap_req.response, hhdm_req.response->offset);

    if (!fb_req.response || fb_req.response->framebuffer_count < 1)
        cpu_halt();

    struct limine_framebuffer* lfb = fb_req.response->framebuffers[0];
    fb_init(lfb);
    fb_clear(COLOR_BG);

    kprintf(COL_BOLD "KyronixOS" COL_RST " kernel is starting up " COL_GRN
                     "kyronixos-0.0.1 (x86_64)" COL_RST "\n\n");

    kstatus("Initialising PMM", true);
    vmm_init();
    kstatus("Initialising VMM", true);
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
    vfs_init();
    kstatus("Mounting /proc", vfs_lookup("/proc") != NULL);
    kstatus("Mounting /sys", vfs_lookup("/sys") != NULL);
    kstatus("Mounting /dev/pts", vfs_lookup("/dev/pts") != NULL);
    pci_enumerate();
    kstatus("Enumerating PCI", true);
    uio_init();
    kstatus("Initialising UIO", true);
    fbdev_init();
    kstatus("Registering framebuffer", vfs_lookup("/dev/fb0") != NULL);

    input_init();
    kstatus("Initialising evdev",
            vfs_lookup("/dev/input/event0") != NULL && vfs_lookup("/dev/input/event1") != NULL);
    vt_init();
    kstatus("Initialising virtual tty", true);
    pit_init();
    kstatus("Starting timer", true);
    sti();
    ps2mouse_init();
    kstatus("Initialising PS/2 mouse", true);
    kprintf("\n");

    {
        void* p[4];
        bool pmm_ok = true;
        for (int i = 0; i < 4; i++) {
            p[i] = pmm_alloc();
            if (!p[i]) {
                pmm_ok = false;
                break;
            }
            volatile uint64_t* v = phys_to_virt((uint64_t) p[i]);
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
            if (p[i])
                pmm_free(p[i]);

        log_info("pmm test : %s", pmm_ok ? "PASS" : "FAIL");
    }

    {
        const uint64_t test_virt = 0xffff900000001000ULL;
        uint64_t test_phys = (uint64_t) pmm_alloc();
        bool vmm_ok = false;

        if (test_phys) {
            int r = vmm_map(&g_kernel_space, test_virt, test_phys, VMM_KDATA);
            if (r == 0) {
                volatile uint64_t* p = (volatile uint64_t*) test_virt;
                *p = 0xC0FFEE00DEADC0DEULL;
                vmm_ok = (*p == 0xC0FFEE00DEADC0DEULL);
                vmm_unmap(&g_kernel_space, test_virt);
            }
            pmm_free((void*) test_phys);
        }

        log_info("vmm test : %s", vmm_ok ? "PASS" : "FAIL");
    }

    {
        bool heap_ok = true;

        uint8_t* a = kmalloc(64);
        uint8_t* b = kmalloc(128);
        uint8_t* c = kmalloc(256);

        heap_ok = a && b && c && (a != b) && (b != c) && (a != c);

        if (heap_ok) {
            memset(a, 0xAA, 64);
            memset(b, 0xBB, 128);
            memset(c, 0xCC, 256);
            for (int i = 0; i < 64 && heap_ok; i++)
                if (a[i] != 0xAA)
                    heap_ok = false;
            for (int i = 0; i < 128 && heap_ok; i++)
                if (b[i] != 0xBB)
                    heap_ok = false;
            for (int i = 0; i < 256 && heap_ok; i++)
                if (c[i] != 0xCC)
                    heap_ok = false;
        }

        kfree(b);
        uint8_t* d = kmalloc(64);
        heap_ok = heap_ok && (d != NULL);
        if (d)
            memset(d, 0xDD, 64);

        uint8_t* a2 = krealloc(a, 512);
        heap_ok = heap_ok && (a2 != NULL);
        if (a2) {
            for (int i = 0; i < 64 && heap_ok; i++)
                if (a2[i] != 0xAA)
                    heap_ok = false;
            kfree(a2);
        } else {
            kfree(a);
        }
        kfree(c);
        kfree(d);

        log_info("heap test: %s", heap_ok ? "PASS" : "FAIL");
        heap_stats();
    }

    if (hhdm_req.response)
        log_info("hhdm offset : 0x%016lx", hhdm_req.response->offset);

    log_info("fb : %lux%lu  %u bpp", lfb->width, lfb->height, (unsigned) lfb->bpp);

    print_memmap();

    if (mod_req.response && mod_req.response->module_count > 0) {
        struct limine_file* initrd = mod_req.response->modules[0];
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

    {
        vfs_node_t* init_node = vfs_lookup("/init");
        if (!init_node)
            init_node = vfs_lookup("/sbin/init");
        if (!init_node)
            init_node = vfs_lookup("/bin/init");

        if (init_node && init_node->type == VFS_TYPE_REG && init_node->data) {
            if (process_exec(init_node->data, init_node->size, "/init") < 0)
                kprintf("  FATAL: process_exec failed\n");
        } else {
            kprintf("  FATAL: /init not found in initrd\n");
        }
    }

    fb_set_color(COLOR_GRAY, COLOR_BG);
    kprintf("  Kernel halted.\n");
    cpu_halt();
}
