#pragma once
#include <stdbool.h>
#include <stdint.h>

void kbd_init(void);
bool kbd_data_ready(void);
int kbd_getchar(void);

extern void (*g_kbd_evdev_hook)(uint16_t linuxkey, int value);
uint16_t kbd_set1_to_linuxkey(uint8_t sc, bool ext);
