#pragma once
#include <stdbool.h>
#include <stdint.h>

void kbd_init(void);
bool kbd_data_ready(void);
int kbd_getchar(void);

extern void (*g_kbd_evdev_hook)(uint16_t linuxkey, int value);
