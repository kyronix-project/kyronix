#include "acpi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/cpu.h"
#include "../bus/pci/pci.h"
#include "lib/log.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

struct acpi_rsdp {
    char signature[8];
    uint8_t checksum; // covers the first 20 bytes
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_addr;

    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t ext_checksum; // covers the structure
    uint8_t reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_gas {
    uint8_t address_space; // 0 = SystemMemory, 1 = SystemIO, 2 = PCIConfig
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed));

#define ACPI_AS_MEM 0
#define ACPI_AS_IO 1
#define ACPI_AS_PCI 2

struct acpi_fadt {
    struct acpi_sdt_header hdr;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint8_t cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t reserved1;
    uint32_t flags;
    struct acpi_gas reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    struct acpi_gas x_pm1a_evt_blk;
    struct acpi_gas x_pm1b_evt_blk;
    struct acpi_gas x_pm1a_cnt_blk;
    struct acpi_gas x_pm1b_cnt_blk;
    // other fields unused
} __attribute__((packed));

// FADT.flags bit 10: reset register supported
#define FADT_RESET_REG_SUP (1u << 10)

// PM1 control register bits
#define SLP_EN (1u << 13)
#define SCI_EN (1u << 0)
#define SLP_TYP_SHIFT 10

static bool g_acpi_ok = false;

static uint32_t g_pm1a_cnt = 0; // I/O port of PM1a control block
static uint32_t g_pm1b_cnt = 0; // I/O port of PM1b control block (0 = none)
static uint32_t g_smi_cmd = 0;
static uint8_t g_acpi_enable = 0;
static uint16_t g_sci_int = 0;

static bool g_reset_sup = false;
static struct acpi_gas g_reset_reg;
static uint8_t g_reset_value = 0;

static bool g_s5_found = false;
static uint8_t g_slp_typ_a = 0;
static uint8_t g_slp_typ_b = 0;

static uint8_t acpi_checksum(const void *ptr, size_t len) {
    const uint8_t *p = ptr;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += p[i];
    return sum;
}

static void *acpi_map_range(uint64_t phys, uint64_t len) {
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end = (phys + len + 0xFFF) & ~0xFFFULL;
    for (uint64_t pa = start; pa < end; pa += 0x1000) {
        uint64_t va = (uint64_t) (uintptr_t) phys_to_virt(pa);
        if (!vmm_virt_to_phys(&g_kernel_space, va)) vmm_map(&g_kernel_space, va, pa, VMM_KDATA);
    }
    return phys_to_virt(phys);
}

static const struct acpi_sdt_header *acpi_map_table(uint64_t phys) {
    const struct acpi_sdt_header *h = acpi_map_range(phys, sizeof(struct acpi_sdt_header));
    if (h->length > sizeof(struct acpi_sdt_header)) acpi_map_range(phys, h->length);
    return h;
}

static void *acpi_map(uint64_t phys) { return acpi_map_range(phys, 0x1000); }

// scan dsdt aml
static int aml_decode_int(const uint8_t *p, const uint8_t *end, uint8_t *out) {
    if (p >= end) return -1;
    switch (*p) {
    case 0x00: /* ZeroOp */
        *out = 0;
        return 1;
    case 0x01: /* OneOp */
        *out = 1;
        return 1;
    case 0x0A: /* BytePrefix */
        if (p + 1 >= end) return -1;
        *out = p[1];
        return 2;
    case 0xFF: /* OnesOp */
        *out = 0xFF;
        return 1;
    default:
        return -1;
    }
}

static void acpi_parse_s5(const struct acpi_sdt_header *dsdt) {
    const uint8_t *body = (const uint8_t *) dsdt + sizeof(*dsdt);
    const uint8_t *end = (const uint8_t *) dsdt + dsdt->length;

    for (const uint8_t *p = body; p + 4 < end; p++) {
        if (p[0] != '_' || p[1] != 'S' || p[2] != '5' || p[3] != '_') continue;

        const uint8_t *q = p + 4;
        if (q >= end || *q != 0x12) continue; // PackageOp
        q++;

        /* top two bits of the first byte give the number of
                following length bytes; we only need to skip it. */
        if (q >= end) continue;
        uint8_t lead = *q;
        int extra = lead >> 6;
        q += 1 + extra;
        if (q >= end) continue;

        // NumElements byte
        q++;
        if (q >= end) continue;

        uint8_t a = 0, b = 0;
        int n = aml_decode_int(q, end, &a);
        if (n < 0) continue;
        q += n;
        (void) aml_decode_int(q, end, &b); // second element is optional

        g_slp_typ_a = a & 0x7;
        g_slp_typ_b = b & 0x7;
        g_s5_found = true;
        return;
    }
}

static void acpi_parse_fadt(const struct acpi_fadt *fadt) {
    g_pm1a_cnt = fadt->pm1a_cnt_blk;
    g_pm1b_cnt = fadt->pm1b_cnt_blk;
    if (fadt->hdr.length >= offsetof(struct acpi_fadt, x_pm1b_cnt_blk) + sizeof(struct acpi_gas)) {
        if (fadt->x_pm1a_cnt_blk.address != 0 && fadt->x_pm1a_cnt_blk.address_space == ACPI_AS_IO)
            g_pm1a_cnt = (uint32_t) fadt->x_pm1a_cnt_blk.address;
        if (fadt->x_pm1b_cnt_blk.address != 0 && fadt->x_pm1b_cnt_blk.address_space == ACPI_AS_IO)
            g_pm1b_cnt = (uint32_t) fadt->x_pm1b_cnt_blk.address;
    }

    g_smi_cmd = fadt->smi_cmd;
    g_acpi_enable = fadt->acpi_enable;
    g_sci_int = fadt->sci_int;

    // reset register
    if (fadt->hdr.length >= offsetof(struct acpi_fadt, reset_value) + sizeof(uint8_t) &&
        (fadt->flags & FADT_RESET_REG_SUP) && fadt->reset_reg.address != 0) {
        g_reset_sup = true;
        g_reset_reg = fadt->reset_reg;
        g_reset_value = fadt->reset_value;
    }

    uint64_t dsdt_phys = fadt->dsdt;
    if (fadt->hdr.length >= offsetof(struct acpi_fadt, x_dsdt) + sizeof(uint64_t) &&
        fadt->x_dsdt != 0)
        dsdt_phys = fadt->x_dsdt;
    if (dsdt_phys) {
        const struct acpi_sdt_header *dsdt = acpi_map_table(dsdt_phys);
        if (memcmp(dsdt->signature, "DSDT", 4) == 0) acpi_parse_s5(dsdt);
    }
}

// enable acpi via smi
static void acpi_enable_mode(void) {
    if (!g_pm1a_cnt || !g_smi_cmd || !g_acpi_enable) return;
    if (inw((uint16_t) g_pm1a_cnt) & SCI_EN) return;

    outb((uint16_t) g_smi_cmd, g_acpi_enable);
    for (int i = 0; i < 300000; i++) {
        if (inw((uint16_t) g_pm1a_cnt) & SCI_EN) break;
        io_wait();
    }
}

static void acpi_iterate(uint64_t sdt_phys, bool use_xsdt) {
    const struct acpi_sdt_header *root = acpi_map_table(sdt_phys);
    if (acpi_checksum(root, root->length) != 0) {
        log_warn("ACPI: root table (%s) checksum bad", use_xsdt ? "XSDT" : "RSDT");
        return;
    }

    size_t entry_size = use_xsdt ? 8 : 4;
    size_t count = (root->length - sizeof(*root)) / entry_size;
    const uint8_t *entries = (const uint8_t *) root + sizeof(*root);

    const struct acpi_fadt *fadt = NULL;
    int madt_found = 0;

    for (size_t i = 0; i < count; i++) {
        uint64_t phys;
        if (use_xsdt) {
            uint64_t v;
            memcpy(&v, entries + i * 8, 8);
            phys = v;
        } else {
            uint32_t v;
            memcpy(&v, entries + i * 4, 4);
            phys = v;
        }
        const struct acpi_sdt_header *h = acpi_map_table(phys);
        if (acpi_checksum(h, h->length) != 0) continue;

        if (memcmp(h->signature, "FACP", 4) == 0)
            fadt = (const struct acpi_fadt *) h;
        else if (memcmp(h->signature, "APIC", 4) == 0)
            madt_found = 1;
    }

    log_info("ACPI: %s with %lu tables, MADT %s", use_xsdt ? "XSDT" : "RSDT", (unsigned long) count,
             madt_found ? "present" : "absent");

    if (!fadt) {
        log_warn("ACPI: no FADT found");
        return;
    }

    acpi_parse_fadt(fadt);
    acpi_enable_mode();
    g_acpi_ok = true;

    log_info("ACPI: PM1a_CNT=0x%x PM1b_CNT=0x%x SCI=%u reset=%s S5=%s(a=%u b=%u)", g_pm1a_cnt,
             g_pm1b_cnt, g_sci_int, g_reset_sup ? "yes" : "no", g_s5_found ? "yes" : "def",
             g_slp_typ_a, g_slp_typ_b);
}

void acpi_init(uint64_t rsdp_phys) {
    if (!rsdp_phys) {
        log_warn("ACPI: no RSDP from bootloader");
        return;
    }

    const struct acpi_rsdp *rsdp = acpi_map_range(rsdp_phys, sizeof(struct acpi_rsdp));
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) {
        log_warn("ACPI: bad RSDP signature");
        return;
    }
    if (acpi_checksum(rsdp, 20) != 0) {
        log_warn("ACPI: RSDP checksum bad");
        return;
    }

    if (rsdp->revision >= 2 && rsdp->xsdt_addr != 0) {
        if (acpi_checksum(rsdp, rsdp->length) != 0) {
            log_warn("ACPI: extended RSDP checksum bad, falling back to RSDT");
            acpi_iterate(rsdp->rsdt_addr, false);
        } else {
            acpi_iterate(rsdp->xsdt_addr, true);
        }
    } else {
        acpi_iterate(rsdp->rsdt_addr, false);
    }
}

bool acpi_available(void) { return g_acpi_ok; }

__attribute__((noreturn)) void acpi_poweroff(void) {
    if (g_pm1a_cnt) {
        outw((uint16_t) g_pm1a_cnt, (uint16_t) ((g_slp_typ_a << SLP_TYP_SHIFT) | SLP_EN));
        if (g_pm1b_cnt)
            outw((uint16_t) g_pm1b_cnt, (uint16_t) ((g_slp_typ_b << SLP_TYP_SHIFT) | SLP_EN));
        for (volatile int i = 0; i < 1000000; i++) io_wait();
    }

    // qemu fallback
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);

    cli();
    for (;;) hlt();
}

static void acpi_write_reset_reg(void) {
    switch (g_reset_reg.address_space) {
    case ACPI_AS_IO:
        outb((uint16_t) g_reset_reg.address, g_reset_value);
        break;
    case ACPI_AS_MEM: {
        volatile uint8_t *reg = acpi_map(g_reset_reg.address);
        *reg = g_reset_value;
        break;
    }
    case ACPI_AS_PCI: {
        /* gas pciconf address: bits 32-47 device, 16-31 function,
                        0-15 offset; bus/segment assumed 0. */
        uint64_t a = g_reset_reg.address;
        uint8_t dev = (uint8_t) (a >> 32);
        uint8_t fn = (uint8_t) (a >> 16);
        uint8_t off = (uint8_t) a;
        uint8_t aligned = off & 0xFC;
        uint32_t shift = (off & 3) * 8;
        uint32_t v = pci_read32(0, dev, fn, aligned);
        v = (v & ~(0xFFu << shift)) | ((uint32_t) g_reset_value << shift);
        pci_write32(0, dev, fn, aligned, v);
        break;
    }
    default:
        break;
    }
}

__attribute__((noreturn)) void acpi_reboot(void) {
    // fadt res reg
    if (g_reset_sup) {
        acpi_write_reset_reg();
        for (volatile int i = 0; i < 1000000; i++) io_wait();
    }

    // 8042 kbct
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0) break;
        io_wait();
    }
    outb(0x64, 0xFE);
    for (volatile int i = 0; i < 1000000; i++) io_wait();

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idtr = { 0, 0 };
    __asm__ volatile("lidt %0" ::"m"(null_idtr) : "memory");
    __asm__ volatile("int3");
    cli();
    for (;;) hlt();
}
