.code64
.section .text

#define PROC_SPACE      16
#define PROC_KSTACK_TOP 40
#define PROC_KSTACK_RSP 48
#define PROC_USER_RSP   56
#define PROC_FS_BASE    96

#define CPU_KERNEL_RSP  0
#define CPU_USER_RSP    8

.global sched_switch
sched_switch:
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    movq    g_current_proc(%rip), %r15

    /* save FS base of outgoing process */
    movl    $0xC0000100, %ecx
    rdmsr
    shlq    $32, %rdx
    orq     %rdx, %rax
    movq    %rax, PROC_FS_BASE(%r15)

    movq    %rsp,            PROC_KSTACK_RSP(%r15)

    movq    %gs:CPU_USER_RSP, %rcx
    movq    %rcx,            PROC_USER_RSP(%r15)

    movq    %rdi,            g_current_proc(%rip)

    movq    PROC_USER_RSP(%rdi),    %rcx
    movq    %rcx,                   %gs:CPU_USER_RSP

    movq    PROC_KSTACK_TOP(%rdi),  %rcx
    movq    %rcx,                   %gs:CPU_KERNEL_RSP

    /* restore FS base of incoming process */
    pushq   %rcx
    pushq   %rdx
    movq    PROC_FS_BASE(%rdi),  %rax
    movq    %rax, %rdx
    shrq    $32, %rdx
    movl    $0xC0000100, %ecx
    wrmsr
    popq    %rdx
    popq    %rcx

    movq    PROC_SPACE(%rdi),       %rdi
    call    vmm_switch

    /* rax clobbered by vmm_switch */
    movq    g_current_proc(%rip),   %rax
    movq    PROC_KSTACK_RSP(%rax),  %rsp

    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    retq                            /* retjrn into next's context */


.global proc_resume_frame
proc_resume_frame:
    popq    %rax
    popq    %rbx
    popq    %rcx        /* user RIP */
    popq    %rdx
    popq    %rsi
    popq    %rdi
    popq    %rbp
    popq    %r8
    popq    %r9
    popq    %r10
    popq    %r11        /* user RFLAGS */
    popq    %r12
    popq    %r13
    popq    %r14
    popq    %r15

    movq    %gs:CPU_USER_RSP, %rsp
    swapgs
    sysretq
