#include "smp.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/syscall_setup.h"
#include "boot/limine.h"
#include "fs/vfs.h"
#include "lib/log.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc.h"
#include "syscall/syscall.h"

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SFMASK 0xC0000084

extern void syscall_entry(void);

volatile uint32_t g_cpu_count = 1;
volatile uint32_t g_aps_ready = 0;
volatile uint32_t g_kernel_ready = 0;

static volatile struct limine_smp_request smp_req = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .response = NULL,
    .flags = 0,
};

void smp_init(void) {
    if (!smp_req.response) {
        log_info("SMP: no response from bootloader (UP only)");
        return;
    }

    struct limine_smp_response *resp = smp_req.response;
    g_cpu_count = (uint32_t) resp->cpu_count;

    log_info("SMP: %u CPU(s) detected  BSP lapic=%u", g_cpu_count, resp->bsp_lapic_id);

    uint32_t bsp_lapic = resp->bsp_lapic_id;
    uint32_t next_id = 1;

    for (uint32_t i = 0; i < g_cpu_count; i++) {
        struct limine_smp_info *cpu = resp->cpus[i];
        cpu->extra_argument = 0;
        if (cpu->lapic_id == bsp_lapic) {
            g_cpu_local[0].lapic_id = cpu->lapic_id;
            cpu->extra_argument = (uint64_t) &g_cpu_local[0];
            log_info("SMP:   CPU%u lapic=%u (BSP)", 0, cpu->lapic_id);
        } else {
            uint32_t cid = next_id++;
            if (cid >= MAX_CPUS) break;
            g_cpu_local[cid].cpu_id = cid;
            g_cpu_local[cid].lapic_id = cpu->lapic_id;
            cpu->extra_argument = (uint64_t) &g_cpu_local[cid];
            log_info("SMP:   CPU%u lapic=%u", cid, cpu->lapic_id);
        }
    }
    g_cpu_count = next_id;
}

extern void ap_trampoline(void);

void __attribute__((noreturn)) ap_sched_loop(void) {
    proc_t *idle = g_current_proc;
    for (;;) {
        proc_t *next = sched_claim_next(idle);
        if (next) {
            idle->state = PROC_READY;
            proc_set_ready(idle);
        }

        if (next) {
            vfs_set_fdtable(next->fds);
            g_current_space = next->space;
            cpu_set_kernel_stack(next->kstack_top);
            sched_switch(next);
            idle->state = PROC_RUNNING;
            vfs_set_fdtable(idle->fds);
            g_current_space = idle->space;
            cpu_set_kernel_stack(idle->kstack_top);
        } else {
            __asm__ volatile("sti; hlt" ::: "memory");
        }
    }
}

void __attribute__((noreturn)) ap_init_cpu(cpu_local_t *cpu) {

    gdt_ap_load(cpu->cpu_id);

    idt_load_ap();

    wrmsr(0xC0000101, (uint64_t) cpu);
    wrmsr(0xC0000102, (uint64_t) cpu);

    cpu_enable_sse();

    uint32_t eax = 7, ebx = 0, ecx = 0, edx = 0;
    cpuid(7, &eax, &ebx, &ecx, &edx);
    if (ebx & (1u << 7)) write_cr4(read_cr4() | (1ULL << 20));

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | (1ULL << 0) | (1ULL << 11));
    wrmsr(MSR_STAR,
          ((uint64_t) (GDT_USER_DATA_SEL - 0x8) << 48) | ((uint64_t) GDT_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t) syscall_entry);
    wrmsr(MSR_SFMASK, (1ULL << 9) | (1ULL << 8) | (1ULL << 10) | (1ULL << 18));

    uint32_t svr = lapic_read(LAPIC_SVR) & 0xFFFFFF00;
    lapic_write(LAPIC_SVR, svr | LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VEC);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_EOI, 0);

    __atomic_fetch_add(&g_aps_ready, 1, __ATOMIC_RELEASE);
    cpu->online = 1;

    while (__atomic_load_n(&g_kernel_ready, __ATOMIC_ACQUIRE) == 0) cpu_relax();

    lapic_timer_start_periodic(250);

    ap_sched_loop();
}

void smp_boot_aps(void) {
    if (!smp_req.response) return;

    struct limine_smp_response *resp = smp_req.response;
    uint32_t bsp_lapic = resp->bsp_lapic_id;

    for (uint32_t i = 0; i < resp->cpu_count; i++) {
        struct limine_smp_info *cpu = resp->cpus[i];
        if (cpu->lapic_id == bsp_lapic) continue;
        if (!cpu->extra_argument) continue;
        cpu_local_t *cp = (cpu_local_t *) cpu->extra_argument;
        proc_t *idle = proc_create_idle(cp->cpu_id, ap_sched_loop);
        if (!idle) continue;
        cp->current = idle;
        cp->idle = idle;
    }

    for (uint32_t i = 0; i < resp->cpu_count; i++) {
        struct limine_smp_info *cpu = resp->cpus[i];
        if (cpu->lapic_id == bsp_lapic) continue;
        if (!cpu->extra_argument) continue;
        __atomic_store_n(&cpu->goto_address, ap_trampoline, __ATOMIC_RELEASE);
    }

    uint32_t expected_aps = g_cpu_count - 1;
    while (__atomic_load_n(&g_aps_ready, __ATOMIC_ACQUIRE) < expected_aps) cpu_relax();

    log_info("SMP: all %u AP(s) online, waiting for kernel ready", expected_aps);
}
