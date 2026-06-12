#include "pci.h"
#include "../arch/x86_64/cpu.h"
#include "../lib/log.h"

pci_dev_t g_pci_devs[PCI_MAX_DEVS];
int g_pci_ndevs = 0;

static inline uint32_t cfg_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    return 0x80000000U | ((uint32_t) bus << 16) | ((uint32_t) dev << 11) | ((uint32_t) fn << 8) |
           (reg & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    outl(PCI_CFG_ADDR, cfg_addr(bus, dev, fn, reg));
    return inl(PCI_CFG_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val)
{
    outl(PCI_CFG_ADDR, cfg_addr(bus, dev, fn, reg));
    outl(PCI_CFG_DATA, val);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    uint32_t v = pci_read32(bus, dev, fn, reg & 0xFC);
    return (uint16_t) (v >> ((reg & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    uint32_t v = pci_read32(bus, dev, fn, reg & 0xFC);
    return (uint8_t) (v >> ((reg & 3) * 8));
}

static uint32_t bar_size(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t bar_idx)
{
    uint8_t reg = (uint8_t) (0x10 + bar_idx * 4);
    uint32_t orig = pci_read32(bus, dev, fn, reg);
    pci_write32(bus, dev, fn, reg, 0xFFFFFFFF);
    uint32_t sz = pci_read32(bus, dev, fn, reg);
    pci_write32(bus, dev, fn, reg, orig);
    if (!(sz & ~0xFU))
        return 0;
    return ~(sz & ~0xFU) + 1;
}

static void probe(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t id = pci_read32(bus, dev, fn, 0x00);
    if ((id & 0xFFFF) == 0xFFFF)
        return; /* no device */

    if (g_pci_ndevs >= PCI_MAX_DEVS)
        return;
    pci_dev_t* d = &g_pci_devs[g_pci_ndevs++];
    d->bus = bus;
    d->dev = dev;
    d->fn = fn;
    d->vendor = (uint16_t) (id & 0xFFFF);
    d->device = (uint16_t) (id >> 16);

    uint32_t cls = pci_read32(bus, dev, fn, 0x08);
    d->class = (uint8_t) (cls >> 24);
    d->subclass = (uint8_t) (cls >> 16);
    d->prog_if = (uint8_t) (cls >> 8);

    d->header_type = pci_read8(bus, dev, fn, 0x0E) & 0x7F;

    uint32_t irq = pci_read32(bus, dev, fn, 0x3C);
    d->irq_line = (uint8_t) (irq & 0xFF);
    d->irq_pin = (uint8_t) ((irq >> 8) & 0xFF);

    if (d->header_type == 0)
    { /* normal device: 6 BARs */
        for (int i = 0; i < 6; i++)
        {
            uint32_t bar = pci_read32(bus, dev, fn, (uint8_t) (0x10 + i * 4));
            if (bar & 1)
            { /* I/O BAR — skip */
                d->bars[i] = 0;
                d->bar_sizes[i] = 0;
            }
            else
            {
                int is64 = ((bar >> 1) & 3) == 2;
                uint64_t base = bar & ~0xFULL;
                if (is64 && i < 5)
                {
                    uint32_t hi = pci_read32(bus, dev, fn, (uint8_t) (0x14 + i * 4));
                    base |= (uint64_t) hi << 32;
                    i++; /* 64-bit BAR occupies two slots */
                }
                d->bars[i] = base;
                d->bar_sizes[i] = bar_size(bus, dev, fn, (uint8_t) (i));
            }
        }
    }

    log_info("PCI %02x:%02x.%x  %04x:%04x  class %02x:%02x  irq %u", bus, dev, fn, d->vendor,
             d->device, d->class, d->subclass, d->irq_line);
}

void pci_enumerate(void)
{
    for (uint16_t bus = 0; bus < 256; bus++)
    {
        for (uint8_t dev = 0; dev < 32; dev++)
        {
            uint32_t id = pci_read32((uint8_t) bus, dev, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF)
                continue;
            uint8_t hdr = pci_read8((uint8_t) bus, dev, 0, 0x0E);
            int nfn = (hdr & 0x80) ? 8 : 1; /* multi-function check */
            for (int fn = 0; fn < nfn; fn++)
                probe((uint8_t) bus, dev, (uint8_t) fn);
        }
    }
    log_info("PCI: found %d devices", g_pci_ndevs);
}
