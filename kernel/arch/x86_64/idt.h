#pragma once
#include "cpu.h"
#include <stdint.h>

void idt_init(void);
void isr_dispatch(cpu_state_t* state);
void request_irq(uint8_t irq, void (*fn)(int, void*), void* arg);
