#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PIT_CHANNEL0 0x40
#define PIT_CMD 0x43
#define PIT_DIVISOR 4772
#define PIT_HZ 250
#define PIT_TICK_MS (1000 / PIT_HZ)

extern volatile uint64_t g_ticks;
extern uint64_t g_epoch_base;
void pit_init(void);
uint64_t realtime_now_ms(void);
bool realtime_set_ms(uint64_t value);
