#pragma once
#include <stdint.h>
typedef struct {
    uint64_t sec;
    uint64_t usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
} input_event_t;

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02

#define SYN_REPORT 0x00
#define SYN_DROPPED 0x03

#define REL_X 0
#define REL_Y 1
#define REL_WHEEL 8

#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112

#define INPUT_DEV_KBD 0
#define INPUT_DEV_MOUSE 1
#define INPUT_NDEVS 2

void input_init(void);
void input_push(int dev, uint16_t type, uint16_t code, int32_t value);
void input_watchdog(void); /* timer tick: log wedged evdev readers (diagnostic only) */
extern int g_evdev_kbd_open; /* open-count of /dev/input/event0 - mutes tty echo */
