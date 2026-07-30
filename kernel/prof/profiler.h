#pragma once
#ifdef CONFIG_PROFILER

#include <stdint.h>

#define PROF_STACK_DEPTH 16
#define PROF_RING_SIZE 4096
#define PROF_TOP_N 50

typedef struct {
    uint64_t rip;
    uint32_t pid;
    uint8_t depth;
    uint64_t stack[PROF_STACK_DEPTH];
} prof_sample_t;

typedef struct {
    uint64_t addr;
    const char *name;
    uint64_t count;
} prof_agg_t;

typedef struct {
    uint8_t active;
    uint64_t total_ticks;
    uint64_t dropped;
    uint32_t write_idx;
    uint32_t count;
    prof_sample_t ring[PROF_RING_SIZE];
} prof_ring_t;

void prof_tick(uint64_t rip, uint32_t pid);
void prof_start(void);
void prof_stop(void);
void prof_reset(void);
int prof_is_active(void);
int prof_render(char *buf, uint64_t bufsz);
void prof_print(void);

#endif
