#pragma once
#include "../proc/proc.h"
#include "pci.h"
#include <stdint.h>

typedef struct {
    pci_dev_t* pdev;
    volatile uint32_t irq_count;
    proc_t* waiter;
} uio_dev_t;

void uio_init(void);
