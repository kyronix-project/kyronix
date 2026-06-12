#pragma once
#include <stdint.h>

#define PCI_CFG_ADDR 0xCF8
#define PCI_CFG_DATA 0xCFC

typedef struct
{
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t class, subclass, prog_if;
    uint64_t bars[6];      /* physical base of each BAR (0 = unused/IO) */
    uint32_t bar_sizes[6]; /* size of each BAR region */
    uint8_t irq_line;
    uint8_t irq_pin;
    uint8_t header_type;
} pci_dev_t;

#define PCI_MAX_DEVS 64
extern pci_dev_t g_pci_devs[];
extern int g_pci_ndevs;

void pci_enumerate(void);
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
