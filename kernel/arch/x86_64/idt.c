#include "idt.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/syscall_setup.h"
#include "drivers/fb.h"
#include "exec/process.h"
#include "fs/vfs.h"
#include "gdt.h"
#include "lib/printf.h"
#include "mm/pmm.h"
#include "mm/vma.h"
#include "mm/vmm.h"
#include "net/net.h"
#include "pic.h"
#include "pit.h"
#include "proc/proc.h"
#include "security/phantom.h"
#include "proc/signal.h"
#include "proc/smp.h"
#include "syscall/syscall.h"
#ifdef CONFIG_PROFILER
#include "prof/profiler.h"
#endif

#define IDT_INT_GATE 0x8E
#define IDT_TRAP_GATE 0x8F
#define IDT_USER_GATE 0xEE

static idt_entry_t g_idt[256] __attribute__((aligned(16)));

typedef struct {
    void (*fn)(int, void *);
    void *arg;
    uint32_t active;
} irq_handler_t;
static irq_handler_t g_irq_handlers[16];
static spinlock_t g_irq_handler_lock;

void request_irq(uint8_t irq, void (*fn)(int, void *), void *arg) {
    if (irq < 16) {
        uint64_t flags = irq_save();
        pic_mask_irq(irq);
        spin_lock(&g_irq_handler_lock);
        g_irq_handlers[irq].fn = fn;
        g_irq_handlers[irq].arg = arg;
        spin_unlock(&g_irq_handler_lock);
        pic_unmask_irq(irq);
        irq_restore(flags);
    }
}

bool free_irq(uint8_t irq, void (*fn)(int, void *), void *arg) {
    if (irq >= 16) return false;
    uint64_t flags = irq_save();
    irq_handler_t *handler = &g_irq_handlers[irq];
    spin_lock(&g_irq_handler_lock);
    if (handler->fn != fn || handler->arg != arg) {
        spin_unlock(&g_irq_handler_lock);
        irq_restore(flags);
        return false;
    }
    pic_mask_irq(irq);
    handler->fn = NULL;
    handler->arg = NULL;
    spin_unlock(&g_irq_handler_lock);
    irq_restore(flags);
    while (__atomic_load_n(&handler->active, __ATOMIC_ACQUIRE)) cpu_relax();
    return true;
}

extern uint64_t isr_stub_table[];

static void idt_set_gate(uint8_t vec, uint64_t handler, uint8_t type) {
    g_idt[vec] = (idt_entry_t) {
        .offset_low = (uint16_t) (handler & 0xFFFF),
        .selector = GDT_KERNEL_CODE,
        .ist = 0,
        .type_attr = type,
        .offset_mid = (uint16_t) ((handler >> 16) & 0xFFFF),
        .offset_high = (uint32_t) (handler >> 32),
        .zero = 0,
    };
}

static struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) g_idtr;

void idt_init(void) {
    pic_remap(32, 40);
    pic_mask_all();

    for (uint8_t i = 0; i < 32; i++) idt_set_gate(i, isr_stub_table[i], IDT_INT_GATE);

    idt_set_gate(3, isr_stub_table[3], IDT_TRAP_GATE); /* #BP -  trap so debugger can resume */
    g_idt[8].ist = 1;                                  /* #DF -> g_tss.ist[0] */
    g_idt[2].ist = 2;                                  /* NMI -> g_tss.ist[1] */

    for (uint8_t i = 0; i < 16; i++) idt_set_gate(32 + i, isr_stub_table[32 + i], IDT_INT_GATE);

    idt_set_gate(0x80, isr_stub_table[48], IDT_USER_GATE);

    idt_set_gate(LAPIC_TIMER_VEC, isr_stub_table[49], IDT_INT_GATE);
    idt_set_gate(LAPIC_SPURIOUS_VEC, isr_stub_table[50], IDT_INT_GATE);

    g_idtr.limit = (uint16_t) (sizeof(g_idt) - 1);
    g_idtr.base = (uint64_t) g_idt;
    __asm__ volatile("lidt %0" ::"m"(g_idtr) : "memory");
}

void idt_load_ap(void) { __asm__ volatile("lidt %0" ::"m"(g_idtr) : "memory"); }

static const char *const exc_name[] = {
    "#DE Divide Error",
    "#DB Debug",
    "NMI",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR Bound Range Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "Coproc Segment Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack Segment Fault",
    "#GP General Protection Fault",
    "#PF Page Fault",
    "(reserved 15)",
    "#MF x87 FP Exception",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XF SIMD FP Exception",
    "(reserved 20)",
    "(reserved 21)",
    "(reserved 22)",
    "(reserved 23)",
    "(reserved 24)",
    "(reserved 25)",
    "(reserved 26)",
    "(reserved 27)",
    "(reserved 28)",
    "(reserved 29)",
    "#SX Security Exception",
    "(reserved 31)",
};

typedef enum {
    PF_UNHANDLED = 0,
    PF_HANDLED,
    PF_SIGSEGV,
    PF_SIGBUS,
} page_fault_result_t;

static bool page_fault_stack_growth_ok(cpu_state_t *state, uint64_t page, bool exec) {
    if (exec) return false;
    if (page < USER_STACK_GROW_BASE || page >= USER_STACK_TOP) return false;
    return page + USER_STACK_GROW_SLOP_PAGES * PAGE_SIZE >= state->rsp;
}

static page_fault_result_t handle_user_page_fault(cpu_state_t *state) {
    if (!g_current_proc || !g_current_proc->space) return PF_SIGSEGV;

    if (state->error_code & 0x1) {
        if ((state->error_code & 0x2) &&
            vmm_handle_cow_fault(g_current_proc->space, read_cr2()) > 0)
            return PF_HANDLED;
        return PF_SIGSEGV; /* protection violation */
    }

    uint64_t cr2 = read_cr2();
    uint64_t page = cr2 & ~0xFFFULL;
    bool write = (state->error_code & 0x2) != 0;
    bool exec = (state->error_code & 0x10) != 0;

    if (page_fault_stack_growth_ok(state, page, exec)) {
        void *phys = pmm_alloc_zeroed();
        if (!phys) return PF_SIGBUS;
        if (vmm_map(g_current_proc->space, page, (uint64_t) phys, VMM_UDATA) == 0)
            return PF_HANDLED;
        pmm_free(phys);
        return PF_SIGBUS;
    }

    if (vma_page_fault_allowed(g_current_proc->space, page, write, exec)) {
        void *phys = pmm_alloc_zeroed();
        if (!phys) return PF_SIGBUS;
        uint64_t flags = vma_page_flags(g_current_proc->space, page);
        if (vmm_map(g_current_proc->space, page, (uint64_t) phys, flags) == 0) return PF_HANDLED;
        pmm_free(phys);
        return PF_SIGBUS;
    }

    return PF_SIGSEGV;
}

static int exception_signal(uint64_t n) {
    static const int exc_sig[] = {
        SIGFPE,  SIGFPE, SIGILL,  SIGTRAP, SIGILL, SIGFPE, SIGILL, SIGFPE,  SIGFPE, SIGFPE, SIGFPE,
        SIGTRAP, SIGILL, SIGSEGV, SIGSEGV, SIGFPE, SIGBUS, SIGFPE, SIGTRAP, SIGFPE, SIGFPE, SIGFPE,
        SIGFPE,  SIGFPE, SIGFPE,  SIGFPE,  SIGFPE, SIGFPE, SIGFPE, SIGFPE,  SIGFPE, SIGFPE,
    };
    return (n < 32) ? exc_sig[n] : SIGSEGV;
}

#define KTEXT_LO 0xffffffff80000000ULL
#define KTEXT_HI 0xffffffff80040000ULL

static void kernel_backtrace(uint64_t sp) {
    static volatile int in_bt = 0;
    if (in_bt) return;
    in_bt = 1;
    if (g_current_proc && g_current_proc->kstack_guard) {
        uint64_t lo = g_current_proc->kstack_guard + 0x1000;
        uint64_t top = g_current_proc->kstack_top;
        uint64_t from = (sp + 7) & ~7ULL;
        if (from > lo && from < top) lo = from;
        kprintf("  kstack return-addr scan [0x%016lx..0x%016lx):\n", lo, top);
        int n = 0;
        for (uint64_t a = lo; a < top && n < 48; a += 8) {
            uint64_t v = *(volatile uint64_t *) a;
            if (v >= KTEXT_LO && v < KTEXT_HI) {
                kprintf("    0x%016lx\n", v);
                n++;
            }
        }
    }
    in_bt = 0;
}

void isr_dispatch(cpu_state_t *state) {
    uint64_t n = state->int_no;

    if (n < 32) {
        if ((state->cs & 3) == 3 && g_current_proc) {
            int sig = exception_signal(n);
            if (n == 14) {
                page_fault_result_t pf = handle_user_page_fault(state);
                if (pf == PF_HANDLED) return;
                uint64_t fault_address = read_cr2();
                bool candidate = phantom_fault_candidate(state, fault_address);
                if (candidate && phantom_handle_fault(state, fault_address)) return;
                sig = (pf == PF_SIGBUS) ? SIGBUS : SIGSEGV;
            }

            if ((n == 1 || n == 3) && g_current_proc->tracer_pid) {
                if (n == 3 && state->rip) state->rip--; /* int3 leaves rip past the opcode */
                g_current_proc->ptrace_orig_rax = state->rax;
                proc_ptrace_stop(g_current_proc, SIGTRAP, 2, state, &state->rflags);
                return;
            }

            kdbg("\n[exc#%lu pid=%u RIP=%lx] -> sig %d\n", n, g_current_proc->pid, state->rip, sig);
            if (n == 14) {
                uint64_t cr2 = read_cr2();
                kdbg("  CR2=%lx err=%lx\n", cr2, state->error_code);
            }
            proc_do_exit(-sig);
        }

        kprintf("\n\n!!! KERNEL PANIC !!! pid=%u\n", g_current_proc ? g_current_proc->pid : 0);
        kprintf("  %s  (vector %lu)\n", exc_name[n], n);
        if (n == 8) kprintf("  (double fault - usually a kernel stack overflow)\n");
        kprintf("  error = 0x%016lx\n", state->error_code);
        kprintf("  RIP   = 0x%016lx   CS     = 0x%04lx\n", state->rip, state->cs);
        kprintf("  RFLAGS= 0x%016lx\n", state->rflags);
        kprintf("  RAX   = 0x%016lx   RBX    = 0x%016lx\n", state->rax, state->rbx);
        kprintf("  RCX   = 0x%016lx   RDX    = 0x%016lx\n", state->rcx, state->rdx);
        kprintf("  RSI   = 0x%016lx   RDI    = 0x%016lx\n", state->rsi, state->rdi);
        kprintf("  R8    = 0x%016lx   R9     = 0x%016lx\n", state->r8, state->r9);
        kprintf("  R10   = 0x%016lx   R11    = 0x%016lx\n", state->r10, state->r11);
        kprintf("  R12   = 0x%016lx   R13    = 0x%016lx\n", state->r12, state->r13);
        kprintf("  R14   = 0x%016lx   R15    = 0x%016lx\n", state->r14, state->r15);
        kprintf("  RBP   = 0x%016lx\n", state->rbp);

        if (n == 14) {
            uint64_t cr2 = read_cr2();
            kprintf("  CR2   = 0x%016lx  (fault address)\n", cr2);
            kprintf("  PF flags: %s%s%s%s%s\n", state->error_code & 1 ? "present " : "non-present ",
                    state->error_code & 2 ? "write " : "read ",
                    state->error_code & 4 ? "user " : "kernel ",
                    state->error_code & 8 ? "reserved-bit " : "",
                    state->error_code & 16 ? "ifetch" : "");
        }

        if ((state->cs & 3) == 3)
            kprintf("  RSP   = 0x%016lx   SS     = 0x%04lx  (ring-3)\n", state->rsp, state->ss);
        else
            kernel_backtrace(state->rsp);

        cpu_halt();
    } else if (n < 48) {
        uint8_t irq = (uint8_t) (n - 32);
        if (irq == 0) {
            g_ticks += PIT_TICK_MS;
#ifdef CONFIG_PROFILER
            prof_tick(state->rip, g_current_proc ? g_current_proc->pid : 0);
#endif
            fb_cursor_blink_tick(g_ticks);
            proc_reap_pending();
            net_poll();
            pic_send_eoi(0);
            uint64_t timer_mask = __atomic_load_n(&g_timer_mask, __ATOMIC_RELAXED);
            uint64_t tm = timer_mask;
            while (tm) {
                int i = __builtin_ctzll(tm);
                proc_t *pc = &g_proctable[i];
                if (pc->wakeup_tick && g_ticks >= pc->wakeup_tick) {
                    pc->wakeup_tick = 0;
                    if (__sync_bool_compare_and_swap(&pc->state, PROC_WAITING, PROC_READY))
                        proc_set_ready(pc);
                }
                if (pc->alarm_tick && g_ticks >= pc->alarm_tick) {
                    pc->alarm_tick = 0;
                    proc_send_signal(pc, SIGALRM);
                }
                if (pc->itimer_next_tick && g_ticks >= pc->itimer_next_tick) {
                    pc->itimer_next_tick =
                        pc->itimer_interval_ms ? pc->itimer_next_tick + pc->itimer_interval_ms : 0;
                    proc_send_signal(pc, SIGALRM);
                }
                tm &= tm - 1;
            }
            /* rebuild: clear bits for processes with no pending timers */
            tm = timer_mask;
            uint64_t still_active = 0;
            while (tm) {
                int b = __builtin_ctzll(tm);
                proc_t *pt = &g_proctable[b];
                if (pt->wakeup_tick || pt->alarm_tick || pt->itimer_next_tick)
                    still_active |= (1ULL << b);
                tm &= tm - 1;
            }
            uint64_t finished = timer_mask & ~still_active;
            if (finished)
                __atomic_fetch_and(&g_timer_mask, ~finished, __ATOMIC_RELAXED);
            if ((state->cs & 3) == 3 && g_current_proc) {
                proc_t *p = g_current_proc;
                proc_t *next = sched_claim_next(p);
                if (next) {
                    p->state = PROC_READY;
                    proc_set_ready(p);
                    vfs_set_fdtable(next->fds);
                    g_current_space = next->space;
                    cpu_set_kernel_stack(next->kstack_top);
                    sched_switch(next);
                    p->state = PROC_RUNNING;
                    vfs_set_fdtable(p->fds);
                    g_current_space = p->space;
                    cpu_set_kernel_stack(p->kstack_top);
                }
            }
        } else {
            irq_handler_t *h = &g_irq_handlers[irq];
            void (*fn)(int, void *) = NULL;
            void *arg = NULL;
            spin_lock(&g_irq_handler_lock);
            if (h->fn) {
                fn = h->fn;
                arg = h->arg;
                __atomic_fetch_add(&h->active, 1, __ATOMIC_RELAXED);
            }
            spin_unlock(&g_irq_handler_lock);
            if (fn) {
                fn((int) irq, arg);
                __atomic_fetch_sub(&h->active, 1, __ATOMIC_RELEASE);
            }
            pic_send_eoi(irq);
        }
    } else if (n == LAPIC_SPURIOUS_VEC) {
        return;
    } else if (n == LAPIC_TIMER_VEC) {
        lapic_eoi();
        if ((state->cs & 3) == 3 && g_current_proc) {
            proc_t *p = g_current_proc;
            proc_t *next = sched_claim_next(p);
            if (next) {
                p->state = PROC_READY;
                proc_set_ready(p);
                vfs_set_fdtable(next->fds);
                g_current_space = next->space;
                cpu_set_kernel_stack(next->kstack_top);
                sched_switch(next);
                p->state = PROC_RUNNING;
                vfs_set_fdtable(p->fds);
                g_current_space = p->space;
                cpu_set_kernel_stack(p->kstack_top);
            }
        }
    }
}
