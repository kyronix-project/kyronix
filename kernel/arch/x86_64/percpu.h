#pragma once
#include "cpu.h"
#include <stdint.h>

typedef struct {
    volatile uint64_t lock;
} spinlock_t;

#define SPINLOCK_INIT ((spinlock_t) { .lock = 0 })
#define this_cpu_ptr()                                                                             \
    (__extension__({                                                                               \
        uint32_t __id;                                                                             \
        __asm__ volatile("movl %%gs:16, %0" : "=r"(__id));                                         \
        &g_cpu_local[__id];                                                                        \
    }))

#define per_cpu_field(field)                                                                       \
    (*(__extension__({                                                                             \
        cpu_local_t *__p = this_cpu_ptr();                                                         \
        (volatile __typeof__(__p->field) *) &(__p->field);                                         \
    })))

#define g_current_space per_cpu_field(current_space)

#define g_cur_fds per_cpu_field(g_fds_ptr)

#define g_reap_thread per_cpu_field(reap_thread)

typedef struct {
    uint64_t kernel_rsp;
    uint64_t user_rsp;
    uint32_t cpu_id;
    uint32_t lapic_id;
    uint8_t online;
    uint8_t __pad0[7];
    void *current;
    void *idle;
    void *current_space;
    void *g_fds_ptr;
    void *reap_thread;
    uint64_t current_fs_base;
} cpu_local_t;

_Static_assert(__builtin_offsetof(cpu_local_t, kernel_rsp) == 0, "gs:0");
_Static_assert(__builtin_offsetof(cpu_local_t, user_rsp) == 8, "gs:8");
_Static_assert(__builtin_offsetof(cpu_local_t, cpu_id) == 16, "gs:16");
_Static_assert(__builtin_offsetof(cpu_local_t, current) == 32, "gs:32");
_Static_assert(__builtin_offsetof(cpu_local_t, current_space) == 48, "gs:48");
_Static_assert(__builtin_offsetof(cpu_local_t, g_fds_ptr) == 56, "gs:56");
_Static_assert(__builtin_offsetof(cpu_local_t, reap_thread) == 64, "gs:64");
_Static_assert(__builtin_offsetof(cpu_local_t, current_fs_base) == 72, "gs:72");
_Static_assert(sizeof(cpu_local_t) == 80, "cpu_local_t size");

#define CPU_CURRENT_OFFSET 32

#define MAX_CPUS 16

extern cpu_local_t g_cpu_local[MAX_CPUS];

static inline uint32_t this_cpu_id(void) {
    uint32_t id;
    __asm__ volatile("movl %%gs:16, %0" : "=r"(id));
    return id;
}

static inline uint32_t this_cpu_lapic_id(void) {
    uint32_t id;
    __asm__ volatile("movl %%gs:20, %0" : "=r"(id));
    return id;
}

static inline int cpu_online(uint32_t cpu_id) { return g_cpu_local[cpu_id].online; }

static inline void cpu_set_online(uint32_t cpu_id) { g_cpu_local[cpu_id].online = 1; }
