#include "gdt.h"
#include "cpu.h"

#define MAKE_ACCESS(present, dpl, system, exec, dir_conf, rw) \
    ((present << 7) | ((dpl) << 5) | (system << 4) | (exec << 3) | (dir_conf << 2) | (rw << 1))

#define GDT_LIMIT 0xFFFFF
#define GDT_BASE  0

enum {
    GDT_ACC_RW     = 1,
    GDT_ACC_EXEC   = 1,
    GDT_ACC_SYSTEM = 1,
};

enum {
    GDT_FLG_64BIT   = (1 << 1),
    GDT_FLG_GRAN4K  = (1 << 3),
};

#define GDT_REG_CNT 5

static struct {
    gdt_entry_t      reg[GDT_REG_CNT];
    tss_descriptor_t tss_desc;
} PACKED gdt;

static gdtr_t gdtr;

static void gdt_set_entry(int i, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags)
{
    gdt.reg[i].limit_low   = limit & 0xFFFF;
    gdt.reg[i].base_low    = base & 0xFFFF;
    gdt.reg[i].base_mid    = (base >> 16) & 0xFF;
    gdt.reg[i].access      = access;
    gdt.reg[i].granularity = ((limit >> 16) & 0x0F) | (flags << 4);
    gdt.reg[i].base_high   = (base >> 24) & 0xFF;
}

static void tss_desc_init(const tss_t *tss)
{
    uint64_t base = (uint64_t)tss;
    uint16_t limit = sizeof(tss_t) - 1;

    gdt.tss_desc.limit_low   = limit;
    gdt.tss_desc.base_low    = base & 0xFFFF;
    gdt.tss_desc.base_mid    = (base >> 16) & 0xFF;
    gdt.tss_desc.access      = 0x89;
    gdt.tss_desc.granularity = (limit >> 16) & 0x0F;
    gdt.tss_desc.base_high   = (base >> 24) & 0xFF;
    gdt.tss_desc.base_upper  = (base >> 32) & 0xFFFFFFFF;
    gdt.tss_desc.reserved    = 0;
}

static void gdt_load(void)
{
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)&gdt;

    __asm__ volatile(
        "lgdt %0\n"
        "mov  %1, %%ax\n"
        "mov  %%ax, %%ds\n"
        "mov  %%ax, %%es\n"
        "mov  %%ax, %%fs\n"
        "mov  %%ax, %%gs\n"
        "mov  %%ax, %%ss\n"
        "push %2\n"
        "leaq 1f(%%rip), %%rax\n"
        "push %%rax\n"
        "lretq\n"
        "1:\n"
        : : "m"(gdtr),
            "rm"((uint16_t)GDT_DS_SEL),
            "i"((uint16_t)GDT_CS_SEL)
        : "rax", "memory"
    );
}

static void tss_load(void)
{
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS_SEL) : "memory");
}

static uint8_t tss_stack[4096] ALIGNED(16);
static tss_t   tss;

void gdt_init(void)
{
    gdt_set_entry(GDT_NULL,        0, 0, 0, 0);
    gdt_set_entry(GDT_KERNEL_CODE,
        GDT_BASE, GDT_LIMIT,
        MAKE_ACCESS(1, 0, GDT_ACC_SYSTEM, GDT_ACC_EXEC, 0, GDT_ACC_RW),
        GDT_FLG_64BIT | GDT_FLG_GRAN4K);
    gdt_set_entry(GDT_KERNEL_DATA,
        GDT_BASE, GDT_LIMIT,
        MAKE_ACCESS(1, 0, GDT_ACC_SYSTEM, 0, 0, GDT_ACC_RW),
        GDT_FLG_64BIT | GDT_FLG_GRAN4K);
    gdt_set_entry(GDT_USER_DATA,
        GDT_BASE, GDT_LIMIT,
        MAKE_ACCESS(1, 3, GDT_ACC_SYSTEM, 0, 0, GDT_ACC_RW),
        GDT_FLG_64BIT | GDT_FLG_GRAN4K);
    gdt_set_entry(GDT_USER_CODE,
        GDT_BASE, GDT_LIMIT,
        MAKE_ACCESS(1, 3, GDT_ACC_SYSTEM, GDT_ACC_EXEC, 0, GDT_ACC_RW),
        GDT_FLG_64BIT | GDT_FLG_GRAN4K);

    tss.rsp[0] = (uint64_t)(tss_stack + sizeof(tss_stack));
    tss.iopb_offset = sizeof(tss_t);
    tss_desc_init(&tss);

    gdt_load();
    tss_load();
}
