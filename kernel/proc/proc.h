#pragma once
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "proc/signal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROC_UNUSED 0
#define PROC_RUNNING 1
#define PROC_READY 2
#define PROC_WAITING 3 /* blocked in wait4 */
#define PROC_ZOMBIE 4

#define PROC_MAX 64
#define KSTACK_PAGES 8
#define KSTACK_SIZE (KSTACK_PAGES * 4096ULL)

typedef struct proc
{
    int state;                       /*  0 */
    int pgid;                        /*  4 */
    uint32_t pid;                    /*  8 */
    uint32_t ppid;                   /* 12 */
    vmm_space_t* space;              /* 16 */
    uint64_t kstack_guard;
    uint8_t* kstack;                 /* 24 */
    uint64_t kstack_top;             /* 32 */
    uint64_t kstack_rsp;             /* 40 */
    uint64_t user_rsp;               /* 48 */
    int exit_code;                   /* 56 */
    int wait_for;                    /* 60 — pid to wait for (-1 = any) */
    uint64_t brk;                    /* 64 */
    uint64_t brk_base;               /* 72 */
    uint64_t mmap_bump;              /* 80 */
    uint64_t fs_base;                /* 88 */
    vfs_file_t** fds;                /* 96 */
    uint64_t pending_sigs;
    uint64_t sig_mask;
    k_sigaction_t sig_actions[NSIG];
    char cwd[512];
    uint64_t wakeup_tick;
    uint64_t alarm_tick;
    uint32_t uid;
    uint32_t euid;
    uint32_t suid;
    uint32_t gid;
    uint32_t egid;
    uint32_t sgid;
    bool is_thread;
    uint32_t* cleartid_addr;
} proc_t;

extern proc_t g_proctable[PROC_MAX];
extern proc_t* g_current_proc;

void proc_init(void);
proc_t* proc_alloc(uint32_t ppid);
void proc_kstack_free(proc_t* p);
proc_t* proc_find(uint32_t pid);
proc_t* proc_next_ready(proc_t* skip);
void sched_switch(proc_t* next);
void sched_yield_blocking(void);
extern void proc_resume_frame(void);
__attribute__((noreturn)) void proc_do_exit(int code);
