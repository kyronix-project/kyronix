#include "lapic.h"
#include "cpu.h"
#include "lib/log.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "percpu.h"
#include "pit.h"

static volatile uint32_t *g_lapic = NULL;
static uint32_t g_lapic_timer_freq = 0; /* ticks per ms */

#define LAPIC_VIRT 0xfffffe0000000000ULL

static volatile uint32_t *lapic_reg(uint16_t off) { return &g_lapic[off / 4]; }

void lapic_write(uint16_t reg, uint32_t val) {
    if (g_lapic) *lapic_reg(reg) = val;
}

uint32_t lapic_read(uint16_t reg) {
    if (g_lapic) return *lapic_reg(reg);
    return 0;
}

void lapic_init(void) {
    uint64_t apic_base = rdmsr(IA32_APIC_BASE);

    if (!(apic_base & IA32_APIC_BASE_ENABLE)) {
        wrmsr(IA32_APIC_BASE, apic_base | IA32_APIC_BASE_ENABLE);
    }

    uint64_t lapic_phys = apic_base & 0xFFFFFFFFFFFFF000ULL;

    int r = vmm_map(&g_kernel_space, LAPIC_VIRT, lapic_phys, VMM_KDATA | VMM_PCD);
    if (r < 0) {
        log_warn("LAPIC: failed to map MMIO at %p", (void *) lapic_phys);
        return;
    }
    g_lapic = (volatile uint32_t *) LAPIC_VIRT;

    uint32_t svr = *lapic_reg(LAPIC_SVR) & 0xFFFFFF00;
    *lapic_reg(LAPIC_SVR) = svr | LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC;

    *lapic_reg(LAPIC_LVT_ERROR) = LAPIC_LVT_MASKED;
    *lapic_reg(LAPIC_LVT_THERMAL) = LAPIC_LVT_MASKED;
    *lapic_reg(LAPIC_LVT_PERF) = LAPIC_LVT_MASKED;

    *lapic_reg(LAPIC_TPR) = 0;

    uint32_t id = *lapic_reg(LAPIC_ID) >> 24;
    uint32_t version = *lapic_reg(LAPIC_VERSION) & 0xFF;
    log_info("LAPIC: id=%u version=%u phys=%p", id, version, (void *) lapic_phys);

    g_cpu_local[0].lapic_id = id;

    *lapic_reg(LAPIC_EOI) = 0;
}

uint32_t lapic_read_id(void) {
    if (!g_lapic) return 0;
    return *lapic_reg(LAPIC_ID) >> 24;
}

void lapic_eoi(void) {
    if (g_lapic) *lapic_reg(LAPIC_EOI) = 0;
}

void lapic_send_ipi(uint32_t lapic_id, uint32_t icr_lo) {
    if (!g_lapic) return;

    while (*lapic_reg(LAPIC_ICR_LO) & ICR_SEND_PENDING) cpu_relax();

    *lapic_reg(LAPIC_ICR_HI) = (uint32_t) lapic_id << 24;

    *lapic_reg(LAPIC_ICR_LO) = icr_lo;

    while (*lapic_reg(LAPIC_ICR_LO) & ICR_SEND_PENDING) cpu_relax();
}

void lapic_send_ipi_self(uint32_t icr_lo) { lapic_send_ipi(0, icr_lo | ICR_DEST_SELF); }

static inline uint16_t pit_read_counter(void) {
    outb(PIT_CMD, 0x00); /* latch channel 0 */
    uint8_t lo = inb(PIT_CHANNEL0);
    uint8_t hi = inb(PIT_CHANNEL0);
    return (uint16_t) lo | ((uint16_t) hi << 8);
}

// calibrate lapic
void lapic_calibrate_timer(void) {
    if (!g_lapic) return;

    uint8_t pit_div_lo = inb(PIT_CHANNEL0);
    uint8_t pit_div_hi = inb(PIT_CHANNEL0);
    (void) pit_div_lo;
    (void) pit_div_hi;

    *lapic_reg(LAPIC_TIMER_DIV) = 0x0B;
    *lapic_reg(LAPIC_TIMER_ICOUNT) = 0xFFFFFFFF;

    uint16_t prev = pit_read_counter();
    int ticks = 0;
    while (ticks < 5) {
        uint16_t cur = pit_read_counter();
        if (cur > prev) ticks++;
        prev = cur;
        cpu_relax();
    }

    uint32_t remaining = *lapic_reg(LAPIC_TIMER_CCUR);
    uint32_t count = 0xFFFFFFFF - remaining;
    g_lapic_timer_freq = count / 5;

    *lapic_reg(LAPIC_LVT_TIMER) = LAPIC_LVT_MASKED;
    *lapic_reg(LAPIC_TIMER_ICOUNT) = 0;

    log_info("LAPIC timer: %u ticks/ms  (%u Hz)", g_lapic_timer_freq, g_lapic_timer_freq * 1000);
}

void lapic_timer_start_periodic(uint32_t hz) {
    if (!g_lapic || !g_lapic_timer_freq) return;

    uint32_t count = g_lapic_timer_freq * 1000 / hz;

    *lapic_reg(LAPIC_LVT_TIMER) = LAPIC_TIMER_VEC | LAPIC_TIMER_PERIODIC;
    *lapic_reg(LAPIC_TIMER_DIV) = 0x0B;
    *lapic_reg(LAPIC_TIMER_ICOUNT) = count;
}

uint32_t lapic_timer_freq(void) { return g_lapic_timer_freq; }
