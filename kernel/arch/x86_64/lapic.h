#pragma once
#include <stdint.h>
#define LAPIC_ID 0x020
#define LAPIC_VERSION 0x030
#define LAPIC_TPR 0x080
#define LAPIC_APR 0x090
#define LAPIC_PPR 0x0A0
#define LAPIC_EOI 0x0B0
#define LAPIC_LDR 0x0D0
#define LAPIC_DFR 0x0E0
#define LAPIC_SVR 0x0F0
#define LAPIC_ISR_BASE 0x100
#define LAPIC_TMR_BASE 0x180
#define LAPIC_IRR_BASE 0x200
#define LAPIC_ESR 0x280
#define LAPIC_ICR_LO 0x300
#define LAPIC_ICR_HI 0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_THERMAL 0x330
#define LAPIC_LVT_PERF 0x340
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define LAPIC_TIMER_ICOUNT 0x380
#define LAPIC_TIMER_CCUR 0x390
#define LAPIC_TIMER_DIV 0x3E0

// lvt masks and nodss
#define LAPIC_LVT_MASKED (1 << 16)
#define LAPIC_TIMER_PERIODIC (1 << 17)
#define LAPIC_TIMER_ONESHOT (0 << 17)
#define LAPIC_TIMER_TSC_DEADLINE (1 << 19)

#define LAPIC_SVR_ENABLE (1 << 8)
#define LAPIC_SVR_FOCUS (1 << 9)

#define ICR_FIXED 0
#define ICR_LOWEST_PRI (1 << 8)
#define ICR_SMI (2 << 8)
#define ICR_NMI (4 << 8)
#define ICR_INIT (5 << 8)
#define ICR_STARTUP (6 << 8)
#define ICR_EXTINT (7 << 8)

#define ICR_DEST_PHYSICAL (0 << 11)
#define ICR_DEST_LOGICAL (1 << 11)

#define ICR_SEND_PENDING (1 << 12)
#define ICR_LEVEL_ASSERT (1 << 14)

#define ICR_DEST_SELF (1 << 18)
#define ICR_DEST_ALL (2 << 18)
#define ICR_DEST_OTHERS (3 << 18)

// inter-pt vector
#define LAPIC_SPURIOUS_VEC 0xFF

// timer vector
#define LAPIC_TIMER_VEC 0xE0

// msrs
#define IA32_APIC_BASE 0x0000001B
#define IA32_APIC_BASE_ENABLE (1 << 11)
#define IA32_APIC_BASE_X2APIC (1 << 10)

void lapic_init(void);
void lapic_eoi(void);
uint32_t lapic_read_id(void);
void lapic_send_ipi(uint32_t lapic_id, uint32_t icr_lo);
void lapic_send_ipi_self(uint32_t icr_lo);
void lapic_calibrate_timer(void);
void lapic_timer_start_periodic(uint32_t hz);
uint32_t lapic_timer_freq(void);

void lapic_write(uint16_t reg, uint32_t val);
uint32_t lapic_read(uint16_t reg);
