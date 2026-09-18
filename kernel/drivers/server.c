#include "server.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "acpi.h"

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

struct acpi_rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct mcfg_entry {
    uint64_t base;
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
} __attribute__((packed));

struct madt_header {
    struct acpi_sdt_header hdr;
    uint32_t lapic_addr;
    uint32_t flags;
} __attribute__((packed));

struct madt_ioapic {
    uint8_t type;
    uint8_t length;
    uint8_t apic_id;
    uint8_t reserved;
    uint32_t address;
    uint32_t gsi_base;
} __attribute__((packed));

struct srat_cpu {
    uint8_t type;
    uint8_t length;
    uint8_t domain_lo;
    uint8_t apic_id;
    uint32_t flags;
    uint8_t sapic_eid;
    uint8_t domain_hi[3];
    uint32_t clock_domain;
} __attribute__((packed));

struct srat_mem {
    uint8_t type;
    uint8_t length;
    uint32_t domain;
    uint16_t reserved1;
    uint64_t base;
    uint64_t length_;
    uint32_t reserved2;
    uint32_t flags;
    uint64_t reserved3;
} __attribute__((packed));

static pci_ecam_range_t g_ecam[PCI_ECAM_MAX_SEGMENTS];
static int g_ecam_count;

static numa_mem_range_t g_numa_mem[NUMA_MAX_NODES * 4];
static int g_numa_mem_count;
static numa_cpu_t g_numa_cpu[256];
static int g_numa_cpu_count;
static uint32_t g_numa_domains[NUMA_MAX_NODES];
static int g_numa_node_count;

static ioapic_info_t g_ioapics[IOAPIC_MAX];
static int g_ioapic_count;

#define ECAM_VBASE 0xffff934000000000ULL
#define ECAM_VSIZE 0x0000001000000000ULL

static uint8_t sdt_checksum(const void *ptr, uint64_t len) {
    const uint8_t *p = ptr;
    uint8_t sum = 0;
    for (uint64_t i = 0; i < len; i++) sum += p[i];
    return sum;
}

static void *srv_map_range(uint64_t phys, uint64_t len) {
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end = (phys + len + 0xFFF) & ~0xFFFULL;
    for (uint64_t pa = start; pa < end; pa += 0x1000) {
        uint64_t va = (uint64_t) (uintptr_t) phys_to_virt(pa);
        if (!vmm_virt_to_phys(&g_kernel_space, va)) vmm_map(&g_kernel_space, va, pa, VMM_KDATA);
    }
    return phys_to_virt(phys);
}

static const struct acpi_sdt_header *srv_map_table(uint64_t phys) {
    const struct acpi_sdt_header *h = srv_map_range(phys, sizeof(struct acpi_sdt_header));
    if (h->length > sizeof(struct acpi_sdt_header)) srv_map_range(phys, h->length);
    return h;
}

static void numa_note_domain(uint32_t dom) {
    for (int i = 0; i < g_numa_node_count; i++)
        if (g_numa_domains[i] == dom) return;
    if (g_numa_node_count < NUMA_MAX_NODES) g_numa_domains[g_numa_node_count++] = dom;
}

static void parse_mcfg(const struct acpi_sdt_header *h) {
    const struct mcfg_entry *e =
        (const struct mcfg_entry *) ((const uint8_t *) h + sizeof(*h) + 8);
    int n = (int) ((h->length - sizeof(*h) - 8) / sizeof(*e));
    for (int i = 0; i < n && g_ecam_count < PCI_ECAM_MAX_SEGMENTS; i++) {
        g_ecam[g_ecam_count].base = e[i].base;
        g_ecam[g_ecam_count].segment = e[i].segment;
        g_ecam[g_ecam_count].start_bus = e[i].start_bus;
        g_ecam[g_ecam_count].end_bus = e[i].end_bus;
        log_info("MCFG: seg %u buses %u-%u base 0x%lx", e[i].segment, e[i].start_bus,
                 e[i].end_bus, e[i].base);
        g_ecam_count++;
    }
}

static void parse_madt(const struct acpi_sdt_header *h) {
    const uint8_t *p = (const uint8_t *) h + sizeof(struct madt_header);
    const uint8_t *end = (const uint8_t *) h + h->length;
    while (p + 2 <= end) {
        uint8_t type = p[0];
        uint8_t len = p[1];
        if (len < 2 || p + len > end) break;
        if (type == 1 && len >= sizeof(struct madt_ioapic)) {
            const struct madt_ioapic *ioa = (const struct madt_ioapic *) p;
            if (g_ioapic_count < IOAPIC_MAX) {
                g_ioapics[g_ioapic_count].apic_id = ioa->apic_id;
                g_ioapics[g_ioapic_count].address = ioa->address;
                g_ioapics[g_ioapic_count].gsi_base = ioa->gsi_base;
                log_info("MADT: IOAPIC id=%u addr=0x%lx gsi_base=%u", ioa->apic_id,
                         (uint64_t) ioa->address, ioa->gsi_base);
                g_ioapic_count++;
            }
        }
        p += len;
    }
}

static void parse_srat(const struct acpi_sdt_header *h) {
    const uint8_t *p = (const uint8_t *) h + sizeof(*h) + 12;
    const uint8_t *end = (const uint8_t *) h + h->length;
    while (p + 2 <= end) {
        uint8_t type = p[0];
        uint8_t len = p[1];
        if (len < 2 || p + len > end) break;
        if (type == 0 && len >= sizeof(struct srat_cpu)) {
            const struct srat_cpu *c = (const struct srat_cpu *) p;
            if (g_numa_cpu_count < 256 && (c->flags & 1)) {
                uint32_t dom = c->domain_lo | ((uint32_t) c->domain_hi[0] << 8) |
                               ((uint32_t) c->domain_hi[1] << 16) | ((uint32_t) c->domain_hi[2] << 24);
                g_numa_cpu[g_numa_cpu_count].proximity_domain = dom;
                g_numa_cpu[g_numa_cpu_count].apic_id = c->apic_id;
                g_numa_cpu[g_numa_cpu_count].enabled = true;
                g_numa_cpu_count++;
                numa_note_domain(dom);
            }
        } else if (type == 1 && len >= sizeof(struct srat_mem)) {
            const struct srat_mem *m = (const struct srat_mem *) p;
            if (g_numa_mem_count < NUMA_MAX_NODES * 4 && (m->flags & 1) && m->length_) {
                g_numa_mem[g_numa_mem_count].proximity_domain = m->domain;
                g_numa_mem[g_numa_mem_count].base = m->base;
                g_numa_mem[g_numa_mem_count].length = m->length_;
                log_info("SRAT: mem domain=%u base=0x%lx len=%lu MiB", m->domain, m->base,
                         m->length_ / (1024 * 1024));
                g_numa_mem_count++;
                numa_note_domain(m->domain);
            }
        }
        p += len;
    }
}

int server_tables_init(void) {
    if (!acpi_available()) {
        log_warn("server: ACPI unavailable, skipping MCFG/SRAT/MADT");
        return 0;
    }

    uint64_t rsdp_phys = acpi_rsdp_phys();
    if (!rsdp_phys) return 0;
    const struct acpi_rsdp *rsdp = srv_map_range(rsdp_phys, sizeof(*rsdp));

    uint64_t root_phys;
    bool xsdt;
    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        root_phys = rsdp->xsdt_addr;
        xsdt = true;
    } else {
        root_phys = rsdp->rsdt_addr;
        xsdt = false;
    }
    const struct acpi_sdt_header *root = srv_map_table(root_phys);
    if (sdt_checksum(root, root->length) != 0) return 0;

    uint64_t esz = xsdt ? 8 : 4;
    uint64_t count = (root->length - sizeof(*root)) / esz;
    const uint8_t *entries = (const uint8_t *) root + sizeof(*root);

    for (uint64_t i = 0; i < count; i++) {
        uint64_t phys;
        if (xsdt) memcpy(&phys, entries + i * 8, 8);
        else {
            uint32_t v;
            memcpy(&v, entries + i * 4, 4);
            phys = v;
        }
        const struct acpi_sdt_header *h = srv_map_table(phys);
        if (sdt_checksum(h, h->length) != 0) continue;
        if (!memcmp(h->signature, "MCFG", 4)) parse_mcfg(h);
        else if (!memcmp(h->signature, "APIC", 4)) parse_madt(h);
        else if (!memcmp(h->signature, "SRAT", 4)) parse_srat(h);
    }

    if (g_ecam_count) {
        uint64_t pages = ECAM_VSIZE / PAGE_SIZE;
        if (pages > 4096) pages = 4096;
        for (uint64_t i = 0; i < pages; i++) {
            uint64_t va = ECAM_VBASE + i * PAGE_SIZE;
            uint64_t pa = g_ecam[0].base + i * PAGE_SIZE;
            if (pa < g_ecam[0].base + (uint64_t) (g_ecam[0].end_bus + 1) * 0x100000ULL)
                vmm_map(&g_kernel_space, va, pa, VMM_KDATA | VMM_PCD);
        }
    }

    log_info("server: %d ECAM range(s), %d IOAPIC(s), %d NUMA node(s), %d SRAT cpu(s)",
             g_ecam_count, g_ioapic_count, g_numa_node_count, g_numa_cpu_count);
    return g_ecam_count;
}

int pci_ecam_range_count(void) { return g_ecam_count; }
const pci_ecam_range_t *pci_ecam_range_get(int idx) {
    if (idx < 0 || idx >= g_ecam_count) return NULL;
    return &g_ecam[idx];
}

bool pci_ecam_find(uint8_t bus, uint64_t *base_out) {
    for (int i = 0; i < g_ecam_count; i++) {
        if (bus >= g_ecam[i].start_bus && bus <= g_ecam[i].end_bus) {
            *base_out = g_ecam[i].base;
            return true;
        }
    }
    return false;
}

void *pci_ecam_virt_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg) {
    uint64_t base;
    if (!pci_ecam_find(bus, &base)) return NULL;
    uint64_t off = ((uint64_t) (bus - 0) << 20) | ((uint64_t) dev << 15) |
                   ((uint64_t) fn << 12) | (reg & 0xFFFu);
    uint64_t va = ECAM_VBASE + (off & (ECAM_VSIZE - 1));
    if (!vmm_virt_to_phys(&g_kernel_space, va)) {
        uint64_t pa = base + off;
        uint64_t page_va = va & PAGE_MASK;
        uint64_t page_pa = pa & PAGE_MASK;
        if (!vmm_virt_to_phys(&g_kernel_space, page_va))
            vmm_map(&g_kernel_space, page_va, page_pa, VMM_KDATA | VMM_PCD);
    }
    return (void *) (uintptr_t) va;
}

uint32_t pci_ecam_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg) {
    void *va = pci_ecam_virt_addr(bus, dev, fn, (uint16_t) (reg & 0xFFC));
    if (!va) return 0xFFFFFFFFu;
    return *(volatile uint32_t *) va;
}

void pci_ecam_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg, uint32_t val) {
    void *va = pci_ecam_virt_addr(bus, dev, fn, (uint16_t) (reg & 0xFFC));
    if (!va) return;
    *(volatile uint32_t *) va = val;
}

int numa_node_count(void) { return g_numa_node_count; }
int numa_mem_range_count(void) { return g_numa_mem_count; }
const numa_mem_range_t *numa_mem_range_get(int idx) {
    if (idx < 0 || idx >= g_numa_mem_count) return NULL;
    return &g_numa_mem[idx];
}
int numa_cpu_count(void) { return g_numa_cpu_count; }
const numa_cpu_t *numa_cpu_get(int idx) {
    if (idx < 0 || idx >= g_numa_cpu_count) return NULL;
    return &g_numa_cpu[idx];
}

uint32_t numa_node_of_addr(uint64_t phys) {
    for (int i = 0; i < g_numa_mem_count; i++) {
        if (phys >= g_numa_mem[i].base && phys < g_numa_mem[i].base + g_numa_mem[i].length)
            return g_numa_mem[i].proximity_domain;
    }
    return 0;
}

int system_socket_count(void) {
    int sockets = g_numa_node_count;
    if (sockets < 1) sockets = 1;
    return sockets;
}

int ioapic_count(void) { return g_ioapic_count; }
const ioapic_info_t *ioapic_get(int idx) {
    if (idx < 0 || idx >= g_ioapic_count) return NULL;
    return &g_ioapics[idx];
}

uint64_t ioapic_addr_for_gsi(uint32_t gsi) {
    for (int i = 0; i < g_ioapic_count; i++) {
        if (gsi >= g_ioapics[i].gsi_base) return g_ioapics[i].address;
    }
    return g_ioapic_count ? g_ioapics[0].address : 0;
}
