#pragma once

#include <stdint.h>
#include "cpu.h"

#define GDT_NULL        0
#define GDT_KERNEL_CODE 1
#define GDT_KERNEL_DATA 2
#define GDT_USER_DATA   3
#define GDT_USER_CODE   4
#define GDT_TSS         5

#define GDT_CS_SEL    ((uint16_t)(GDT_KERNEL_CODE << 3))
#define GDT_DS_SEL    ((uint16_t)(GDT_KERNEL_DATA  << 3))
#define GDT_TSS_SEL   ((uint16_t)(GDT_TSS << 3))

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} PACKED tss_descriptor_t;

void gdt_init(void);
