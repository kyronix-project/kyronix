#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "boot/limine.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "drivers/serial.h"
#include "drivers/fb.h"
#include "lib/printf.h"
#include "lib/string.h"

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

LIMINE_REQUESTS_END_MARKER;

static void kernel_putchar(char c, void *ctx) {
    (void)ctx;
    serial_putchar(COM1, c);
    if (g_fb.addr) fb_putchar(c);
}

static const char *memmap_type_name(uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:                 return "Usable";
        case LIMINE_MEMMAP_RESERVED:               return "Reserved";
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "ACPI reclaimable";
        case LIMINE_MEMMAP_ACPI_NVS:               return "ACPI NVS";
        case LIMINE_MEMMAP_BAD_MEMORY:             return "Bad memory";
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "Bootloader reclaimable";
        case LIMINE_MEMMAP_KERNEL_AND_MODULES:     return "Kernel & modules";
        case LIMINE_MEMMAP_FRAMEBUFFER:            return "Framebuffer";
        default:                                   return "Unknown";
    }
}

static void print_banner(void) {
    fb_set_color(COLOR_CYAN, COLOR_BG);
    kprintf("K  K  Y   Y  RRRR    OOO   N   N  III  X   X \n");
    kprintf("K K    Y Y   R   R  O   O  NN  N   I    X X  \n");
    kprintf("KK      Y    RRRR   O   O  N N N   I     X   \n");
    kprintf("K K     Y    R R    O   O  N  NN   I    X X  \n");
    kprintf("K  K    Y    R  RR   OOO   N   N  III  X   X \n");
    kprintf("                                           \n");
    kprintf("                                           \n");

    fb_set_color(COLOR_WHITE, COLOR_BG);
}

static void print_memmap(void) {
    if (!mmap_req.response) {
        kprintf("[warn] no memory map\n");
        return;
    }
    struct limine_memmap_response *resp = mmap_req.response;
    uint64_t total_usable = 0;

    fb_set_color(COLOR_YELLOW, COLOR_BG);
    kprintf("Memory map (%lu entries):\n", resp->entry_count);
    fb_set_color(COLOR_WHITE, COLOR_BG);

    for (uint64_t i = 0; i < resp->entry_count; i++) {
        struct limine_memmap_entry *e = resp->entries[i];
        kprintf("  %016lx-%016lx  %s\n",
                e->base, e->base + e->length,
                memmap_type_name(e->type));
        if (e->type == LIMINE_MEMMAP_USABLE)
            total_usable += e->length;
    }
    kprintf("  Usable: %lu MiB\n\n", total_usable / (1024 * 1024));
}

// kernel entry here

void kmain(void) {
    gdt_init();
    serial_init(COM1);
    printf_set_putchar(kernel_putchar, NULL);

    if (!fb_req.response || fb_req.response->framebuffer_count < 1)
        cpu_halt();

    struct limine_framebuffer *lfb = fb_req.response->framebuffers[0];
    fb_init(lfb);
    fb_clear(COLOR_BG);

    print_banner();

    fb_set_color(COLOR_GREEN, COLOR_BG);
    kprintf("Hello, World!\n\n");
    fb_set_color(COLOR_WHITE, COLOR_BG);

    if (hhdm_req.response)
        kprintf("HHDM offset : 0x%016lx\n", hhdm_req.response->offset);

    kprintf("Framebuffer : %lux%lu  %u bpp\n\n",
            lfb->width, lfb->height, (unsigned)lfb->bpp);

    print_memmap();

    fb_set_color(COLOR_GRAY, COLOR_BG);
    kprintf("Kernel halted.\n");

    cpu_halt();
}
