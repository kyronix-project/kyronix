#ifndef KERNEL_DRIVERS_USB_HID_H
#define KERNEL_DRIVERS_USB_HID_H

#include <stdint.h>
#include <stdbool.h>

#define USB_HID_REPORT_LEN     8
#define USB_HID_MAX_HELD       6
#define USB_HID_REPEAT_INITIAL_NS  250000000ULL
#define USB_HID_REPEAT_INTERVAL_NS  33000000ULL

typedef struct {
    uint8_t  usage;
    uint64_t first_press_ns;
    uint64_t next_emit_ns;
} usb_hid_held_t;

typedef struct {
    uint8_t        prev_report[USB_HID_REPORT_LEN];
    uint8_t        cur_modifier;
    usb_hid_held_t held[USB_HID_MAX_HELD];
} usb_hid_kbd_state_t;

void usb_hid_kbd_state_init(usb_hid_kbd_state_t *s);
void usb_hid_kbd_process_report(usb_hid_kbd_state_t *s, const uint8_t *report);
void usb_hid_kbd_tick_repeats(usb_hid_kbd_state_t **states, int n);

void usb_hid_mouse_process_report(const uint8_t *report, int len);

#endif
