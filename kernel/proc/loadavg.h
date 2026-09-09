#pragma once
#include <stdint.h>

#define LOAD_SHIFT 11
#define LOAD_FIXED (1 << LOAD_SHIFT)

extern uint64_t avenrun[3];
extern volatile uint64_t nr_active;

void loadavg_init(void);
void calc_load_tick(void);
