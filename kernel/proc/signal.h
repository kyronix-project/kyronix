#pragma once
#include <stdint.h>

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGWINCH 28
#define NSIG 64

#define SIG_DFL 0ULL
#define SIG_IGN 1ULL

#define SA_NOCLDSTOP 0x00000001ULL
#define SA_NOCLDWAIT 0x00000002ULL
#define SA_SIGINFO 0x00000004ULL
#define SA_RESTORER 0x04000000ULL
#define SA_RESETHAND 0x80000000ULL
#define SA_NODEFER 0x40000000ULL

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

typedef struct {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer; /* user restorer: calls rt_sigreturn(15) */
    uint64_t sa_mask;     /* extra blocked sigs during handler */
} k_sigaction_t;

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    char _pad[116];
} siginfo_t;

typedef struct {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;    /* 64 */
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;  /* 64 */
    uint64_t rip;                                     /*  8 */
    uint64_t eflags;                                  /*  8 */
    uint16_t cs, gs, fs, ss;                          /*  8 */
    uint64_t err, trapno, oldmask, cr2;               /* 32 */
    uint64_t fpstate; /* NULL = no FP state saved  */ /*  8 */
    uint64_t _reserved[8];                            /* 64 */
} mcontext_t;                                         /* 256 bytes */

typedef struct {
    uint64_t uc_flags;
    uint64_t uc_link;
    uint64_t ss_sp, ss_flags, ss_size;
    mcontext_t uc_mcontext;
    uint64_t uc_sigmask;
} ucontext_t; /* 304 bytes */

typedef struct {
    uint64_t pretcode; /* return addr -> restorer */
    siginfo_t info;    /* 128 bytes              */
    ucontext_t uc;     /* 304 bytes              */
} rt_sigframe_t;       /* 8+128+304 = 440 bytes  */

struct proc;
void proc_send_signal(struct proc* p, int sig);
