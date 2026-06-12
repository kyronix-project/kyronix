#pragma once
#include "mm/vmm.h"
#include <stdbool.h>
#include <stdint.h>

/* highest valid user virtual address + 1 */
#define USER_LIMIT 0x800000000000ULL

extern vmm_space_t* g_current_space;

/* readable user range: in-bounds AND every page mapped user-accessible */
static inline bool uptr_ok(const void* p, uint64_t len)
{
    uint64_t base = (uint64_t) (uintptr_t) p;
    if (base >= USER_LIMIT)
        return false;
    if (len && base > USER_LIMIT - len)
        return false;
    return vmm_user_range_ok(g_current_space, base, len, false);
}

/* writable user range: additionally requires the WRITE bit on every page */
static inline bool uptr_ok_w(const void* p, uint64_t len)
{
    uint64_t base = (uint64_t) (uintptr_t) p;
    if (base >= USER_LIMIT)
        return false;
    if (len && base > USER_LIMIT - len)
        return false;
    return vmm_user_range_ok(g_current_space, base, len, true);
}

/* push order in syscall_entry.S: rax last -> rax at rsp+0 */
typedef struct
{
    uint64_t rax; /* syscall nr on entry; return value on exit */
    uint64_t rbx;
    uint64_t rcx; /* user RIP (saved by SYSCALL) */
    uint64_t rdx; /* arg3 */
    uint64_t rsi; /* arg2 */
    uint64_t rdi; /* arg1 */
    uint64_t rbp;
    uint64_t r8;  /* arg5 */
    uint64_t r9;  /* arg6 */
    uint64_t r10; /* arg4 (linux: r10, not rcx) */
    uint64_t r11; /* user RFLAGS (saved by SYSCALL) */
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} __attribute__((packed)) syscall_frame_t;

void syscall_dispatch(syscall_frame_t* f);

void syscall_set_brk(uint64_t brk_base);
void signal_check(syscall_frame_t* f);
