#include "gdt.h"
#include <stddef.h>

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset; /* beyond tss limit -> no IOPB -> deny all user i/o */
} __attribute__((packed)) tss_t;

typedef struct {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t base_mid;
    uint8_t type;         /* 0x89 = present, DPL=0, type=9 (64-bit TSS avail) */
    uint8_t lim_hi_flags; /* [7:4] flags, [3:0] limit[19:16] */
    uint8_t base_hi1;
    uint32_t base_hi2;
    uint32_t zero;
} __attribute__((packed)) tss_desc_t;

typedef struct {
    uint64_t null;
    uint64_t kernel_code;
    uint64_t kernel_data;
    uint64_t user_data;
    uint64_t user_code;
    tss_desc_t tss;
} __attribute__((packed, aligned(16))) gdt_t;

static gdt_t g_gdt;
static tss_t g_tss __attribute__((aligned(16)));

void gdt_init(void)
{
    g_gdt.null = 0ULL;
    g_gdt.kernel_code = 0x00AF9A000000FFFFULL;
    g_gdt.kernel_data = 0x00CF92000000FFFFULL;
    g_gdt.user_data = 0x00CFF2000000FFFFULL;
    g_gdt.user_code = 0x00AFFA000000FFFFULL;

    g_tss.iopb_offset = (uint16_t) sizeof(tss_t);

    uint64_t base = (uint64_t) &g_tss;
    uint32_t limit = (uint32_t) (sizeof(tss_t) - 1);

    g_gdt.tss = (tss_desc_t) {
        .limit_lo = (uint16_t) (limit & 0xFFFF),
        .base_lo = (uint16_t) (base & 0xFFFF),
        .base_mid = (uint8_t) ((base >> 16) & 0xFF),
        .type = 0x89,
        .lim_hi_flags = (uint8_t) ((limit >> 16) & 0x0F),
        .base_hi1 = (uint8_t) ((base >> 24) & 0xFF),
        .base_hi2 = (uint32_t) (base >> 32),
        .zero = 0,
    };

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr = {
        .limit = (uint16_t) (sizeof(g_gdt) - 1),
        .base = (uint64_t) &g_gdt,
    };

    __asm__ volatile("lgdt   %0                  \n\t"
                     "pushq  $0x08               \n\t" /* kernel CS */
                     "leaq   1f(%%rip), %%rax    \n\t"
                     "pushq  %%rax               \n\t"
                     "lretq                      \n\t" /* flush CS */
                     "1:                         \n\t"
                     "movw   $0x10, %%ax         \n\t" /* kernel DS */
                     "movw   %%ax,  %%ds         \n\t"
                     "movw   %%ax,  %%es         \n\t"
                     "movw   %%ax,  %%ss         \n\t"
                     "xorw   %%ax,  %%ax         \n\t" /* FS=GS=0 (set via msr later) */
                     "movw   %%ax,  %%fs         \n\t"
                     "movw   %%ax,  %%gs         \n\t"
                     "movw   $0x28, %%ax         \n\t" /* tss selector */
                     "ltr    %%ax                \n\t"
                     :
                     : "m"(gdtr)
                     : "rax", "memory");
}

void gdt_set_kernel_stack(uint64_t rsp0)
{
    g_tss.rsp0 = rsp0;
}
