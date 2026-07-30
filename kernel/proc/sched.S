.section .note.GNU-stack,"",@progbits
.code64
.section .text

#define PROC_SPACE      16
#define PROC_PID        8
#define PROC_KSTACK_TOP 32
#define PROC_KSTACK_RSP 40
#define PROC_USER_RSP   48
#define PROC_FS_BASE    96
#define PROC_FPU_STATE  3328

#define CPU_KERNEL_RSP  0
#define CPU_USER_RSP    8
#define CPU_CURRENT     32
#define CPU_FS_BASE     72

.global sched_switch
sched_switch:
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    movq    %gs:CPU_CURRENT, %r15

    // kernel idle tasks never use SIMD, so their dummy state need not be saved
    cmpl    $0, PROC_PID(%r15)
    je      1f
    fxsave64 PROC_FPU_STATE(%r15)
1:

    movq    %rsp,            PROC_KSTACK_RSP(%r15)

    movq    %gs:CPU_USER_RSP, %rcx
    movq    %rcx,            PROC_USER_RSP(%r15)

    movq    %rdi,            %gs:CPU_CURRENT

    movq    PROC_USER_RSP(%rdi),    %rcx
    movq    %rcx,                   %gs:CPU_USER_RSP

    movq    PROC_KSTACK_TOP(%rdi),  %rcx
    movq    %rcx,                   %gs:CPU_KERNEL_RSP

    movq    PROC_FS_BASE(%rdi),  %rax
    cmpq    %gs:CPU_FS_BASE, %rax
    je      2f
    movq    %rax, %gs:CPU_FS_BASE
    movq    %rax, %rdx
    shrq    $32, %rdx
    movl    $0xC0000100, %ecx
    wrmsr
2:

    movq    PROC_SPACE(%rdi),       %rdi
    call    vmm_switch

    movq    %gs:CPU_CURRENT,   %rax
    movq    PROC_KSTACK_RSP(%rax),  %rsp

    cmpl    $0, PROC_PID(%rax)
    je      3f
    fxrstor64 PROC_FPU_STATE(%rax)
3:

    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    retq


.global proc_resume_frame
proc_resume_frame:
    popq    %rax
    popq    %rbx
    popq    %rcx        /* user rip */
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


.global proc_resume_interrupt_frame
proc_resume_interrupt_frame:
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbx
    popq    %rax
    addq    $16, %rsp
    swapgs
    iretq
