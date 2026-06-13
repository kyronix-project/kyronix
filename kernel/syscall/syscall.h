#pragma once
#include "mm/vmm.h"
#include <stdbool.h>
#include <stdint.h>
#define USER_LIMIT 0x800000000000ULL

extern vmm_space_t* g_current_space;

static inline bool uptr_ok(const void* p, uint64_t len)
{
    uint64_t base = (uint64_t) (uintptr_t) p;
    if (base >= USER_LIMIT)
        return false;
    if (len && base > USER_LIMIT - len)
        return false;
    return vmm_user_range_ok(g_current_space, base, len, false);
}

static inline bool uptr_ok_w(const void* p, uint64_t len)
{
    uint64_t base = (uint64_t) (uintptr_t) p;
    if (base >= USER_LIMIT)
        return false;
    if (len && base > USER_LIMIT - len)
        return false;
    return vmm_user_range_ok(g_current_space, base, len, true);
}

typedef struct {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} __attribute__((packed)) syscall_frame_t;

void syscall_dispatch(syscall_frame_t* f);
void syscall_set_brk(uint64_t brk_base);
void signal_check(syscall_frame_t* f);
