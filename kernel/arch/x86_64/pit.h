#pragma once
#include <stdint.h>

extern volatile uint64_t g_ticks;
extern uint64_t g_epoch_base;
void pit_init(void);
