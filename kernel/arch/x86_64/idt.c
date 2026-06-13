#include "idt.h"
#include "arch/x86_64/syscall_setup.h"
#include "drivers/fb.h"
#include "exec/process.h"
#include "fs/vfs.h"
#include "gdt.h"
#include "lib/printf.h"
#include "mm/pmm.h"
#include "mm/vma.h"
#include "mm/vmm.h"
#include "pic.h"
#include "pit.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "syscall/syscall.h"

#define IDT_INT_GATE 0x8E
#define IDT_TRAP_GATE 0x8F
#define IDT_USER_GATE 0xEE /* DPL=3: int 0x80 callable from ring 3 */

static idt_entry_t g_idt[256] __attribute__((aligned(16)));

typedef struct {
    void (*fn)(int, void*);
    void* arg;
} irq_handler_t;
static irq_handler_t g_irq_handlers[16];

void request_irq(uint8_t irq, void (*fn)(int, void*), void* arg)
{
    if (irq < 16) {
        g_irq_handlers[irq].fn = fn;
        g_irq_handlers[irq].arg = arg;
        pic_unmask_irq(irq);
    }
}

extern uint64_t isr_stub_table[];

static void idt_set_gate(uint8_t vec, uint64_t handler, uint8_t type)
{
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

void idt_init(void)
{
    pic_remap(32, 40);
    pic_mask_all();

    for (uint8_t i = 0; i < 32; i++)
        idt_set_gate(i, isr_stub_table[i], IDT_INT_GATE);

    idt_set_gate(3, isr_stub_table[3], IDT_TRAP_GATE); /* #BP: trap so debugger can resume */

    for (uint8_t i = 0; i < 16; i++)
        idt_set_gate(32 + i, isr_stub_table[32 + i], IDT_INT_GATE);

    idt_set_gate(0x80, isr_stub_table[48], IDT_USER_GATE);

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = {
        .limit = (uint16_t) (sizeof(g_idt) - 1),
        .base = (uint64_t) g_idt,
    };
    __asm__ volatile("lidt %0" ::"m"(idtr) : "memory");
}

static const char* const exc_name[] = {
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

static bool page_fault_stack_growth_ok(cpu_state_t* state, uint64_t page, bool exec)
{
    if (exec)
        return false;
    if (page < USER_STACK_GROW_BASE || page >= USER_STACK_TOP)
        return false;
    return page + USER_STACK_GROW_SLOP_PAGES * PAGE_SIZE >= state->rsp;
}

static page_fault_result_t handle_user_page_fault(cpu_state_t* state)
{
    if (!g_current_proc || !g_current_proc->space)
        return PF_SIGSEGV;

    if (state->error_code & 0x1)
        return PF_SIGSEGV; /* protection violation */

    uint64_t cr2 = read_cr2();
    uint64_t page = cr2 & ~0xFFFULL;
    bool write = (state->error_code & 0x2) != 0;
    bool exec = (state->error_code & 0x10) != 0;

    if (page_fault_stack_growth_ok(state, page, exec)) {
        void* phys = pmm_alloc_zeroed();
        if (!phys)
            return PF_SIGBUS;
        if (vmm_map(g_current_proc->space, page, (uint64_t) phys, VMM_UDATA) == 0)
            return PF_HANDLED;
        pmm_free(phys);
        return PF_SIGBUS;
    }

    if (vma_page_fault_allowed(g_current_proc->space, page, write, exec)) {
        void* phys = pmm_alloc_zeroed();
        if (!phys)
            return PF_SIGBUS;
        uint64_t flags = vma_page_flags(g_current_proc->space, page);
        if (vmm_map(g_current_proc->space, page, (uint64_t) phys, flags) == 0)
            return PF_HANDLED;
        pmm_free(phys);
        return PF_SIGBUS;
    }

    return PF_SIGSEGV;
}

static int exception_signal(uint64_t n)
{
    static const int exc_sig[] = {
        SIGFPE,  SIGFPE, SIGILL,  SIGTRAP, SIGILL, SIGFPE, SIGILL, SIGFPE,  SIGFPE, SIGFPE, SIGFPE,
        SIGTRAP, SIGILL, SIGSEGV, SIGSEGV, SIGFPE, SIGBUS, SIGFPE, SIGTRAP, SIGFPE, SIGFPE, SIGFPE,
        SIGFPE,  SIGFPE, SIGFPE,  SIGFPE,  SIGFPE, SIGFPE, SIGFPE, SIGFPE,  SIGFPE, SIGFPE,
    };
    return (n < 32) ? exc_sig[n] : SIGSEGV;
}

void isr_dispatch(cpu_state_t* state)
{
    uint64_t n = state->int_no;

    if (n < 32) {
        /* user-mode exception: kill the process, dont halt the kernel */
        if ((state->cs & 3) == 3 && g_current_proc) {
            int sig = exception_signal(n);
            if (n == 14) {
                page_fault_result_t pf = handle_user_page_fault(state);
                if (pf == PF_HANDLED)
                    return;
                sig = (pf == PF_SIGBUS) ? SIGBUS : SIGSEGV;
            }
            kdbg("\n[exc#%lu pid=%u RIP=%lx] -> sig %d\n", n, g_current_proc->pid, state->rip, sig);
            if (n == 14) {
                uint64_t cr2 = read_cr2();
                kdbg("  CR2=%lx err=%lx\n", cr2, state->error_code);
            }
            proc_do_exit(-sig);
        }

        kprintf("\n\n!!! KERNEL EXCEPTION !!! pid=%u\n", g_current_proc ? g_current_proc->pid : 0);
        kprintf("  %s  (vector %lu)\n", exc_name[n], n);
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

        cpu_halt();
    } else if (n < 48) {
        uint8_t irq = (uint8_t) (n - 32);
        if (irq == 0) {
            g_ticks++;
            fb_cursor_blink_tick(g_ticks);
            pic_send_eoi(0);
            for (int i = 0; i < PROC_MAX; i++) {
                proc_t* pc = &g_proctable[i];
                if (pc->state == PROC_UNUSED)
                    continue;
                if (pc->wakeup_tick && g_ticks >= pc->wakeup_tick) {
                    pc->wakeup_tick = 0;
                    if (pc->state == PROC_WAITING)
                        pc->state = PROC_READY;
                }
                if (pc->alarm_tick && g_ticks >= pc->alarm_tick) {
                    pc->alarm_tick = 0;
                    proc_send_signal(pc, SIGALRM);
                    if (pc->state == PROC_WAITING)
                        pc->state = PROC_READY;
                }
                if (pc->itimer_next_tick && g_ticks >= pc->itimer_next_tick) {
                    pc->itimer_next_tick =
                        pc->itimer_interval_ms ? pc->itimer_next_tick + pc->itimer_interval_ms : 0;
                    proc_send_signal(pc, SIGALRM);
                    if (pc->state == PROC_WAITING)
                        pc->state = PROC_READY;
                }
            }
            if ((state->cs & 3) == 3 && g_current_proc) {
                proc_t* p = g_current_proc;
                proc_t* next = proc_next_ready(p);
                if (next) {
                    p->state = PROC_READY;
                    next->state = PROC_RUNNING;
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
            irq_handler_t* h = &g_irq_handlers[irq];
            if (h->fn)
                h->fn((int) irq, h->arg);
            pic_send_eoi(irq);
        }
    }
}
