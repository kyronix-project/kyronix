#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/spinlock.h"
#include "../lib/log.h"
#include "../lib/string.h"

#define EPERM 1
#define EIO 5
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EINVAL 22
#define ENOSPC 28
#define ENOMEM 12
#define ENODEV 19

#define serial_printf(fmt, ...) klog_printf(KLOG_DEBUG, fmt, ##__VA_ARGS__)
#define serial_writestring(s) klog_printf(KLOG_DEBUG, "%s", (s))
#define printf(fmt, ...) klog_printf(KLOG_INFO, fmt, ##__VA_ARGS__)

#include "../arch/x86_64/pit.h"
static inline uint64_t usb_now_ns(void) { return g_ticks * 1000000ULL; }

void usb_msleep(uint64_t ms);
void usb_udelay(uint64_t us);

#include "../mm/pmm.h"

void *dma_alloc_coherent(size_t size, uintptr_t *phys_out);
void *dma_alloc_coherent_low(size_t size, uintptr_t *phys_out);
void dma_free_coherent(void *virt, size_t size);

volatile void *usb_mmio_map_region(uintptr_t phys, size_t size);
void usb_mmio_unmap_region(volatile void *virt, size_t size);

static inline volatile void *mmio_map(uintptr_t phys, size_t size) {
    return usb_mmio_map_region(phys, size);
}

#include "../bus/pci/pci.h"

typedef void (*usb_pci_probe_fn)(pci_dev_t *dev);
int usb_pci_for_each(uint8_t class_, uint8_t subclass, int prog_if,
                     usb_pci_probe_fn probe);

static inline uint32_t pci_read_dword(pci_dev_t *d, uint8_t reg) {
    return pci_read32(d->bus, d->dev, d->fn, reg);
}
static inline void pci_write_dword(pci_dev_t *d, uint8_t reg, uint32_t val) {
    pci_write32(d->bus, d->dev, d->fn, reg, val);
}

#include "../proc/proc.h"

#define task_create(name, fn, arg) proc_create_kernel((name), (void (*)(void)) (fn))

#include "../input/input.h"
#include "../input/kbd.h"

void usb_input_report_key(uint8_t set1_scancode, bool ext, int pressed);

void tty_usb_input_char(uint8_t c);

void usb_mouse_inject_rel(int32_t dx, int32_t dy, bool btn_left, bool btn_right,
                          bool btn_middle, int32_t wheel);
