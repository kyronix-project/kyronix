#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PCI_ECAM_MAX_SEGMENTS 8
#define NUMA_MAX_NODES 8
#define IOAPIC_MAX 8
#define PCI_MAX_DEVS_SERVER 256

typedef struct {
    uint64_t base;
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
} pci_ecam_range_t;

typedef struct {
    uint32_t proximity_domain;
    uint64_t base;
    uint64_t length;
} numa_mem_range_t;

typedef struct {
    uint32_t proximity_domain;
    uint32_t apic_id;
    bool enabled;
} numa_cpu_t;

typedef struct {
    uint8_t apic_id;
    uint64_t address;
    uint32_t gsi_base;
} ioapic_info_t;

int server_tables_init(void);
int pci_ecam_range_count(void);
const pci_ecam_range_t *pci_ecam_range_get(int idx);
bool pci_ecam_find(uint8_t bus, uint64_t *base_out);
void *pci_ecam_virt_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg);
uint32_t pci_ecam_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg);
void pci_ecam_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t reg, uint32_t val);

int numa_node_count(void);
int numa_mem_range_count(void);
const numa_mem_range_t *numa_mem_range_get(int idx);
int numa_cpu_count(void);
const numa_cpu_t *numa_cpu_get(int idx);
uint32_t numa_node_of_addr(uint64_t phys);
int system_socket_count(void);

int ioapic_count(void);
const ioapic_info_t *ioapic_get(int idx);
uint64_t ioapic_addr_for_gsi(uint32_t gsi);
