#pragma once
#include "../proc/proc.h"
#include "../bus/pci/pci.h"
#include "../arch/x86_64/spinlock.h"
#include <stdint.h>

typedef struct {
    pci_dev_t *pdev;
    volatile uint32_t irq_count;
    proc_t *waiter;
    spinlock_irqsave_t lock;
} uio_dev_t;

void uio_init(void);
