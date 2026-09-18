#include "tty.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/pit.h"
#include "../proc/proc.h"
#include "../proc/signal.h"
#include "fb.h"
#include "input.h"
#include "kbd.h"
#include "serial.h"

#define EINTR 4
#define TTY_BUF_SIZE 256

static uint8_t tty_buf[TTY_BUF_SIZE];
static volatile int tty_buf_head;
static volatile int tty_buf_tail;
static int tty_fg_pgid = 1;
static proc_t *tty_waiter;

static struct termios_s tty_termios = {
    .c_iflag = ICRNL | IXON,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CS8 | CREAD,
    .c_lflag = ISIG | ICANON,
    .c_cc = {
        [VINTR] = 0x03,  /* Ctrl-C */
        [VQUIT] = 0x1C,  /* Ctrl-\ */
        [VERASE] = 0x7F, /* DEL */
        [VKILL] = 0x15,  /* Ctrl-U */
        [VEOF] = 0x04,   /* Ctrl-D */
        [VSUSP] = 0x1A,  /* Ctrl-Z */
        [VMIN] = 1,
        [VTIME] = 0,
    },
};

static bool tty_buf_empty(void) { return tty_buf_head == tty_buf_tail; }

static bool tty_buf_full(void) { return ((tty_buf_head + 1) % TTY_BUF_SIZE) == tty_buf_tail; }

static void tty_enqueue(uint8_t c) {
    int next = (tty_buf_head + 1) % TTY_BUF_SIZE;
    if (next != tty_buf_tail) {
        tty_buf[tty_buf_head] = c;
        tty_buf_head = next;
    }
    if (tty_waiter && tty_waiter->state == PROC_WAITING) {
        tty_waiter->state = PROC_READY;
        proc_set_ready(tty_waiter);
    }
    tty_waiter = NULL;
}

static int tty_dequeue(void) {
    if (tty_buf_empty()) return -1;
    uint8_t c = tty_buf[tty_buf_tail];
    tty_buf_tail = (tty_buf_tail + 1) % TTY_BUF_SIZE;
    return (int) c;
}

static void tty_send_sig_pgid(int sig) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (g_proctable[i].state == PROC_UNUSED) continue;
        if (g_proctable[i].pgid == tty_fg_pgid) proc_send_signal(&g_proctable[i], sig);
    }
}

static void tty_input_char(uint8_t c) {
    if (tty_termios.c_lflag & ISIG) {
        int sig = 0;
        if (c == tty_termios.c_cc[VINTR])
            sig = SIGINT;
        else if (c == tty_termios.c_cc[VQUIT])
            sig = SIGQUIT;
        else if (c == tty_termios.c_cc[VSUSP])
            sig = SIGTSTP;

        if (sig) {
            tty_putchar('^');
            tty_putchar((char) ('@' + c));
            tty_putchar('\n');
            tty_send_sig_pgid(sig);
            return;
        }
    }

    /* IGNCR: discard \r */
    if ((tty_termios.c_iflag & IGNCR) && c == '\r') return;

    /* INLCR: map \n -> \r */
    if ((tty_termios.c_iflag & INLCR) && c == '\n') c = '\r';

    /* ICRNL: map \r -> \n (default) */
    if ((tty_termios.c_iflag & ICRNL) && c == '\r') c = '\n';

    /* ISTRIP: mask to 7 bits */
    if (tty_termios.c_iflag & ISTRIP) c &= 0x7F;

    /* ECHO */
    if (tty_termios.c_lflag & ECHO) {
        if (c == '\r') {
            tty_putchar('\r');
            tty_putchar('\n');
        } else if (c == '\n') {
            tty_putchar('\n');
        } else if (c == 0x7F || c == '\b') {
            tty_putchar('\b');
            tty_putchar(' ');
            tty_putchar('\b');
        } else {
            tty_putchar((char) c);
        }
    }

    if (!tty_buf_full()) tty_enqueue(c);
}

void tty_process_input(void) {
    uint64_t flags = irq_save();
#ifdef CONFIG_SERIAL_CONSOLE
    if (serial_data_ready(COM1)) {
        uint8_t c = serial_getchar(COM1);
        tty_input_char(c);
    }
#endif

    if (kbd_data_ready()) {
        int c = kbd_getchar(); /* always drain ps/2 buffer; evdev hook fires inside */
        if (c > 0 && !g_evdev_kbd_open) tty_input_char((uint8_t) c);
    }
    irq_restore(flags);
}

int64_t tty_read(char *buf, uint64_t len) {
    if (!len) return 0;

    uint64_t vmin = tty_termios.c_cc[VMIN];
    if (vmin == 0) vmin = 1;
    if (vmin > len) vmin = len;

    uint64_t i = 0;

    for (;;) {
        if (i >= len) break;

        tty_process_input();

        int c = tty_dequeue();
        if (c >= 0) {
            if ((tty_termios.c_lflag & ICANON) && c == tty_termios.c_cc[VEOF]) {
                if (i == 0) return 0;
                break;
            }
            buf[i++] = (char) c;
            continue;
        }

        /* in raw mode (VMIN=1, VTIME=0): return as soon as we have >= VMIN bytes */
        if (i >= vmin) break;

        if (g_current_proc) {
            uint64_t actionable = g_current_proc->pending_sigs & ~g_current_proc->sig_mask;
            while (actionable) {
                int idx = __builtin_ctzll(actionable);
                actionable &= ~(1ULL << idx);
                if (g_current_proc->sig_actions[idx].sa_handler != SIG_IGN) return -(int64_t) EINTR;
                /* signal is ignored - consume it and keep reading */
                g_current_proc->pending_sigs &= ~(1ULL << idx);
            }
        }

        tty_waiter = g_current_proc;
        if (g_current_proc) g_current_proc->wakeup_tick = g_ticks + 1;
        if (g_current_proc) proc_set_timer(g_current_proc);
        sched_yield_blocking();
        if (g_current_proc) g_current_proc->wakeup_tick = 0;
        if (tty_waiter == g_current_proc) tty_waiter = NULL;
        cpu_relax();
    }

    return (int64_t) i;
}

int64_t tty_write(const char *buf, uint64_t len) {
    tty_process_input();
    for (uint64_t i = 0; i < len; i++) {
        char c = buf[i];

        /* ONLCR: map \n -> \r\n on output */
        if ((tty_termios.c_oflag & ONLCR) && c == '\n') {
#ifdef CONFIG_SERIAL_CONSOLE
            serial_putchar(COM1, '\r');
#endif
            if (g_fb.addr) fb_putchar('\r');
        }

#ifdef CONFIG_SERIAL_CONSOLE
        serial_putchar(COM1, c);
#endif
        if (g_fb.addr) fb_putchar(c);
    }
    return (int64_t) len;
}

bool tty_data_ready(void) {
#ifdef CONFIG_SERIAL_CONSOLE
    return !tty_buf_empty() || serial_data_ready(COM1) || kbd_data_ready();
#else
    return !tty_buf_empty() || kbd_data_ready();
#endif
}

void tty_putchar(char c) {
    if ((tty_termios.c_oflag & ONLCR) && c == '\n') {
#ifdef CONFIG_SERIAL_CONSOLE
        serial_putchar(COM1, '\r');
#endif
        if (g_fb.addr) fb_putchar('\r');
    }
#ifdef CONFIG_SERIAL_CONSOLE
    serial_putchar(COM1, c);
#endif
    if (g_fb.addr) fb_putchar(c);
}

void tty_check_signals(void) { tty_process_input(); }

int tty_get_fg_pgid(void) { return tty_fg_pgid; }

void tty_set_fg_pgid(int pgid) { tty_fg_pgid = pgid; }

uint32_t tty_get_lflag(void) { return tty_termios.c_lflag; }

void tty_set_lflag(uint32_t lflag) { tty_termios.c_lflag = lflag; }

void tty_get_termios(struct termios_s *t) { *t = tty_termios; }

void tty_set_termios(const struct termios_s *t) { tty_termios = *t; }
