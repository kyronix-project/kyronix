#pragma once
#include "pci.h"
#include "../proc/proc.h"
#include <stdint.h>

typedef struct {
    pci_dev_t* pdev;
    volatile uint32_t irq_count; /* incremented by IRQ handler */
    proc_t*   waiter;            /* process blocked in read() */
} uio_dev_t;

void uio_init(void);
