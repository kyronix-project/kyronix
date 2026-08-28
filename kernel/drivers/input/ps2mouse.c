#include "ps2mouse.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/idt.h"
#include "../lib/log.h"
#include "input.h"

#define KBD_DATA 0x60
#define KBD_STAT 0x64
#define KBS_OBF (1u << 0)
#define KBS_IBF (1u << 1)
#define KBS_AUXB (1u << 5) /* output from aux device */

static uint8_t g_pkt[3];
static int g_pkt_idx;
static uint8_t g_btns_prev;

static void ps2_wait_write(void) {
    int timeout = 100000;
    while ((inb(KBD_STAT) & KBS_IBF) && timeout-- > 0);
}

static void ps2_wait_read(void) {
    int timeout = 100000;
    while (!(inb(KBD_STAT) & KBS_OBF) && timeout-- > 0);
}

static void aux_write(uint8_t byte) {
    ps2_wait_write();
    outb(KBD_STAT, 0xD4); /* write to aux port */
    ps2_wait_write();
    outb(KBD_DATA, byte);
}

static void mouse_irq(int irq, void *arg) {
    (void) irq;
    (void) arg;
    if (!(inb(KBD_STAT) & KBS_OBF)) return;
    if (!(inb(KBD_STAT) & KBS_AUXB)) return; /* not from mouse */

    uint8_t byte = inb(KBD_DATA);

    /* resync: byte 0 of every packet must have bit 3 set; drop stray bytes */
    if (g_pkt_idx == 0 && !(byte & 0x08)) return;

    g_pkt[g_pkt_idx++] = byte;
    if (g_pkt_idx < 3) return;
    g_pkt_idx = 0;

    uint8_t flags = g_pkt[0];
    /* X/Y overflow bits set -> movement value is garbage, discard the packet */
    if (flags & 0xC0) return;

    /* 9-bit twos-complement deltas: high (sign) bit lives in the flags byte */
    int dx = g_pkt[1] - ((flags & 0x10) ? 256 : 0);
    int dy = g_pkt[2] - ((flags & 0x20) ? 256 : 0);
    dy = -dy; /* Y is inverted vs screen coordinates */

    if (dx) input_push(INPUT_DEV_MOUSE, EV_REL, REL_X, dx);
    if (dy) input_push(INPUT_DEV_MOUSE, EV_REL, REL_Y, dy);

    uint8_t btns = flags & 0x07;
    uint8_t changed = btns ^ g_btns_prev;
    g_btns_prev = btns;
    if (changed & 1) input_push(INPUT_DEV_MOUSE, EV_KEY, BTN_LEFT, (btns & 1) ? 1 : 0);
    if (changed & 2) input_push(INPUT_DEV_MOUSE, EV_KEY, BTN_RIGHT, (btns & 2) ? 1 : 0);
    if (changed & 4) input_push(INPUT_DEV_MOUSE, EV_KEY, BTN_MIDDLE, (btns & 4) ? 1 : 0);

    input_push(INPUT_DEV_MOUSE, EV_SYN, 0, 0);
}

void ps2mouse_init(void) {
    ps2_wait_write();
    outb(KBD_STAT, 0xA8);

    ps2_wait_write();
    outb(KBD_STAT, 0x20);
    ps2_wait_read();
    uint8_t cfg = inb(KBD_DATA);
    cfg |= 0x02;  /* enable aux IRQ */
    cfg &= ~0x20; /* enable aux clock */
    ps2_wait_write();
    outb(KBD_STAT, 0x60);
    ps2_wait_write();
    outb(KBD_DATA, cfg);

    aux_write(0xFF);
    ps2_wait_read();
    inb(KBD_DATA); /* ACK */
    ps2_wait_read();
    inb(KBD_DATA); /* 0xAA */
    ps2_wait_read();
    inb(KBD_DATA); /* 0x00 */

    aux_write(0xF6);
    ps2_wait_read();
    inb(KBD_DATA); /* ACK */

    aux_write(0xF4);
    ps2_wait_read();
    inb(KBD_DATA); /* ACK */

    g_pkt_idx = 0;
    g_btns_prev = 0;

    request_irq(12, mouse_irq, NULL);
    log_info("PS/2 mouse: enabled (IRQ 12)");
}
