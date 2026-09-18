#include "kbd.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/idt.h"
#include "../drivers/tty.h"
#include "../lib/log.h"

#define KBD_DATA 0x60
#define KBD_STAT 0x64
#define KBS_OBF (1u << 0)
#define KBS_IBF (1u << 1)  /* input buffer full: controller not ready for a write */
#define KBS_AUXB (1u << 5) /* output byte is from the aux (mouse) port */

/* scancode set 1 -> Linux keycode (0 = no mapping) */
static const uint16_t sc_linuxkey[128] = {
    [0x01] = 1,  [0x02] = 2,  [0x03] = 3,  [0x04] = 4,  [0x05] = 5,  [0x06] = 6,  [0x07] = 7,
    [0x08] = 8,  [0x09] = 9,  [0x0A] = 10, [0x0B] = 11, [0x0C] = 12, [0x0D] = 13, [0x0E] = 14,
    [0x0F] = 15, [0x10] = 16, [0x11] = 17, [0x12] = 18, [0x13] = 19, [0x14] = 20, [0x15] = 21,
    [0x16] = 22, [0x17] = 23, [0x18] = 24, [0x19] = 25, [0x1A] = 26, [0x1B] = 27, [0x1C] = 28,
    [0x1D] = 29, [0x1E] = 30, [0x1F] = 31, [0x20] = 32, [0x21] = 33, [0x22] = 34, [0x23] = 35,
    [0x24] = 36, [0x25] = 37, [0x26] = 38, [0x27] = 39, [0x28] = 40, [0x29] = 41, [0x2A] = 42,
    [0x2B] = 43, [0x2C] = 44, [0x2D] = 45, [0x2E] = 46, [0x2F] = 47, [0x30] = 48, [0x31] = 49,
    [0x32] = 50, [0x33] = 51, [0x34] = 52, [0x35] = 53, [0x36] = 54, [0x37] = 55, [0x38] = 56,
    [0x39] = 57, [0x3A] = 58, [0x3B] = 59, [0x3C] = 60, [0x3D] = 61, [0x3E] = 62, [0x3F] = 63,
    [0x40] = 64, [0x41] = 65, [0x42] = 66, [0x43] = 67, [0x44] = 68, [0x45] = 69, [0x46] = 70,
    [0x47] = 71, [0x48] = 72, [0x49] = 73, [0x4A] = 74, [0x4B] = 75, [0x4C] = 76, [0x4D] = 77,
    [0x4E] = 78, [0x4F] = 79, [0x50] = 80, [0x51] = 81, [0x52] = 82, [0x53] = 83, [0x57] = 87,
    [0x58] = 88,
};

/* extended (0xE0 prefix) scancode -> Linux keycode */
static const uint16_t sc_ext_linuxkey[128] = {
    [0x1C] = 96,  [0x1D] = 97,  [0x35] = 98,  [0x38] = 100, [0x47] = 102, [0x48] = 103,
    [0x49] = 104, [0x4B] = 105, [0x4D] = 106, [0x4F] = 107, [0x50] = 108, [0x51] = 109,
    [0x52] = 110, [0x53] = 111, [0x5B] = 125, [0x5C] = 126,
};

void (*g_kbd_evdev_hook)(uint16_t linuxkey, int value); /* set by input.c */

static bool g_shift, g_ctrl, g_alt, g_caps;
static bool g_ext; /* got 0xE0 prefix, next byte completes the scan code */

static const char *g_ext_seq;
static int g_ext_seq_idx;

static const char seq_up[] = { 0x1B, '[', 'A', 0 };
static const char seq_down[] = { 0x1B, '[', 'B', 0 };
static const char seq_right[] = { 0x1B, '[', 'C', 0 };
static const char seq_left[] = { 0x1B, '[', 'D', 0 };
static const char seq_home[] = { 0x1B, '[', 'H', 0 };
static const char seq_end[] = { 0x1B, '[', 'F', 0 };
static const char seq_pgup[] = { 0x1B, '[', '5', '~', 0 };
static const char seq_pgdn[] = { 0x1B, '[', '6', '~', 0 };
static const char seq_ins[] = { 0x1B, '[', '2', '~', 0 };
static const char seq_del[] = { 0x1B, '[', '3', '~', 0 };

static const char sc_ascii[128] = {
    [0x01] = 0x1B, [0x02] = '1',  [0x03] = '2',  [0x04] = '3',  [0x05] = '4',  [0x06] = '5',
    [0x07] = '6',  [0x08] = '7',  [0x09] = '8',  [0x0A] = '9',  [0x0B] = '0',  [0x0C] = '-',
    [0x0D] = '=',  [0x0E] = 0x08, [0x0F] = 0x09, [0x10] = 'q',  [0x11] = 'w',  [0x12] = 'e',
    [0x13] = 'r',  [0x14] = 't',  [0x15] = 'y',  [0x16] = 'u',  [0x17] = 'i',  [0x18] = 'o',
    [0x19] = 'p',  [0x1A] = '[',  [0x1B] = ']',  [0x1C] = 0x0A, [0x1E] = 'a',  [0x1F] = 's',
    [0x20] = 'd',  [0x21] = 'f',  [0x22] = 'g',  [0x23] = 'h',  [0x24] = 'j',  [0x25] = 'k',
    [0x26] = 'l',  [0x27] = ';',  [0x28] = '\'', [0x29] = '`',  [0x2B] = '\\', [0x2C] = 'z',
    [0x2D] = 'x',  [0x2E] = 'c',  [0x2F] = 'v',  [0x30] = 'b',  [0x31] = 'n',  [0x32] = 'm',
    [0x33] = ',',  [0x34] = '.',  [0x35] = '/',  [0x37] = '*',  [0x39] = ' ',  [0x4A] = '-',
    [0x4E] = '+',
};

static const char sc_shift_ascii[128] = {
    [0x01] = 0x1B, [0x02] = '!',  [0x03] = '@',  [0x04] = '#',  [0x05] = '$', [0x06] = '%',
    [0x07] = '^',  [0x08] = '&',  [0x09] = '*',  [0x0A] = '(',  [0x0B] = ')', [0x0C] = '_',
    [0x0D] = '+',  [0x0E] = 0x08, [0x0F] = 0x09, [0x10] = 'Q',  [0x11] = 'W', [0x12] = 'E',
    [0x13] = 'R',  [0x14] = 'T',  [0x15] = 'Y',  [0x16] = 'U',  [0x17] = 'I', [0x18] = 'O',
    [0x19] = 'P',  [0x1A] = '{',  [0x1B] = '}',  [0x1C] = 0x0A, [0x1E] = 'A', [0x1F] = 'S',
    [0x20] = 'D',  [0x21] = 'F',  [0x22] = 'G',  [0x23] = 'H',  [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L',  [0x27] = ':',  [0x28] = '"',  [0x29] = '~',  [0x2B] = '|', [0x2C] = 'Z',
    [0x2D] = 'X',  [0x2E] = 'C',  [0x2F] = 'V',  [0x30] = 'B',  [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<',  [0x34] = '>',  [0x35] = '?',  [0x37] = '*',  [0x39] = ' ', [0x4A] = '-',
    [0x4E] = '+',
};

static void kbd_irq1(int irq, void *arg) {
    (void) irq;
    (void) arg;
    tty_process_input(); /* drain kbd port into tty buffer (feeds evdev hook too) */
}

static void kbd_wait_write(void) {
    int t = 100000;
    while ((inb(KBD_STAT) & KBS_IBF) && t-- > 0) cpu_relax();
}

static bool kbd_wait_read(void) {
    int t = 100000;
    while (!(inb(KBD_STAT) & KBS_OBF) && t-- > 0) cpu_relax();
    return (inb(KBD_STAT) & KBS_OBF) != 0;
}

static void kbd_flush(void) {
    int t = 1000;
    while ((inb(KBD_STAT) & KBS_OBF) && t-- > 0) inb(KBD_DATA);
}

/* send a command byte to the keyboard device, returning its ACK/resend result */
static int kbd_dev_cmd(uint8_t cmd) {
    for (int attempt = 0; attempt < 3; attempt++) {
        kbd_wait_write();
        outb(KBD_DATA, cmd);
        if (!kbd_wait_read()) return -1;
        uint8_t r = inb(KBD_DATA);
        if (r == 0xFA) return 0; /* ACK */
        if (r == 0xFE) continue; /* resend */
        return -1;
    }
    return -1;
}

void kbd_init(void) {
    kbd_wait_write();
    outb(KBD_STAT, 0xAD);
    kbd_wait_write();
    outb(KBD_STAT, 0xA7);
    kbd_flush();

    kbd_wait_write();
    outb(KBD_STAT, 0x20); /* read config */
    uint8_t cfg = kbd_wait_read() ? inb(KBD_DATA) : 0;
    cfg |= 0x01;  /* enable keyboard IRQ1 */
    cfg |= 0x40;  /* enable scancode translation -> set 1 (decoder assumes set 1) */
    cfg &= ~0x10; /* clear keyboard clock-disable bit */
    kbd_wait_write();
    outb(KBD_STAT, 0x60); /* write config */
    kbd_wait_write();
    outb(KBD_DATA, cfg);

    kbd_wait_write();
    outb(KBD_STAT, 0xAE); /* enable keyboard port */

    if (kbd_dev_cmd(0xFF) == 0) kbd_wait_read(), inb(KBD_DATA); /* consume 0xAA self-test result */
    kbd_dev_cmd(0xF4);

    kbd_flush();

    g_ext_seq = NULL;
    g_ext_seq_idx = -1;

    /* irq1 handler: pumps kbd data even when no process reads /dev/tty */
    request_irq(1, kbd_irq1, NULL);
    log_info("PS/2 keyboard: enabled (cfg=0x%02x)", cfg);
}

int kbd_getchar(void) {
    if (g_ext_seq_idx >= 0) {
        char c = g_ext_seq[g_ext_seq_idx];
        if (c == '\0') {
            g_ext_seq = NULL;
            g_ext_seq_idx = -1;
            return -1;
        }
        g_ext_seq_idx++;
        return (int) (unsigned char) c;
    }

    uint8_t st = inb(KBD_STAT);
    if (!(st & KBS_OBF)) return -1;
    if (st & KBS_AUXB) return -1; /* mouse byte - leave it for the IRQ12 handler */

    int raw = inb(KBD_DATA);
    if (raw < 0) return -1;

    uint8_t sc = (uint8_t) raw;

    /* extended prefix (arrow keys ...) */
    if (sc == 0xE0) {
        g_ext = true;
        return -1;
    }

    bool release = sc & 0x80;
    sc &= 0x7F;

    if (g_kbd_evdev_hook) {
        uint16_t lk = g_ext ? sc_ext_linuxkey[sc] : sc_linuxkey[sc];
        if (lk) g_kbd_evdev_hook(lk, release ? 0 : 1);
    }

    if (release) {
        switch (sc) {
        case 0x2A:
        case 0x36:
            g_shift = false;
            break;
        case 0x1D:
            g_ctrl = false;
            break;
        case 0x38:
            g_alt = false;
            break;
        }
        g_ext = false;
        return -1;
    }

    switch (sc) {
    case 0x2A:
    case 0x36:
        g_shift = true;
        return -1;
    case 0x1D:
        g_ctrl = true;
        return -1;
    case 0x38:
        g_alt = true;
        return -1;
    case 0x3A:
        g_caps = !g_caps;
        return -1;
    case 0x3B ... 0x44:
        return -1;
    case 0x45 ... 0x46:
        return -1;
    case 0x47 ... 0x53:
        if (!g_ext) return -1;
        g_ext = false;
        switch (sc) {
        case 0x47:
            g_ext_seq = seq_home;
            break;
        case 0x48:
            g_ext_seq = seq_up;
            break;
        case 0x49:
            g_ext_seq = seq_pgup;
            break;
        case 0x4B:
            g_ext_seq = seq_left;
            break;
        case 0x4D:
            g_ext_seq = seq_right;
            break;
        case 0x4F:
            g_ext_seq = seq_end;
            break;
        case 0x50:
            g_ext_seq = seq_down;
            break;
        case 0x51:
            g_ext_seq = seq_pgdn;
            break;
        case 0x52:
            g_ext_seq = seq_ins;
            break;
        case 0x53:
            g_ext_seq = seq_del;
            break;
        default:
            g_ext_seq = NULL;
        }
        if (g_ext_seq) {
            g_ext_seq_idx = 0;
            char c = g_ext_seq[g_ext_seq_idx++];
            return (int) (unsigned char) c;
        }
        return -1;
    case 0x57 ... 0x58:
        return -1;
    }

    char c;
    if (g_shift)
        c = sc_shift_ascii[sc];
    else
        c = sc_ascii[sc];

    if (c >= 'a' && c <= 'z' && g_caps)
        c = (char) (c - 'a' + 'A');
    else if (c >= 'A' && c <= 'Z' && g_caps)
        c = (char) (c - 'A' + 'a');

    if (g_ctrl && c >= 'a' && c <= 'z')
        c = (char) (c - 'a' + 1);
    else if (g_ctrl && c >= 'A' && c <= 'Z')
        c = (char) (c - 'A' + 1);

    g_ext = false;
    return (int) (unsigned char) c;
}

bool kbd_data_ready(void) {
    if (g_ext_seq_idx >= 0) return true;
    uint8_t st = inb(KBD_STAT);
    return (st & KBS_OBF) && !(st & KBS_AUXB);
}
