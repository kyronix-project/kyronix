#include "input.h"
#include "../arch/x86_64/pit.h"
#include "../fs/vfs.h"
#include "../lib/log.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../proc/proc.h"
#include "../syscall/syscall.h"
#include "../syscall/poll.h"
#include "kbd.h"
#include <stdbool.h>

#define EINVAL 22
#define ENOSYS 38
#define EINTR 4

#define EVBUF 64
#define WD_GRACE_TICKS ((uint64_t) (3000 / PIT_TICK_MS))
#define WD_REMIND_TICKS ((uint64_t) (30000 / PIT_TICK_MS))
typedef struct {
    input_event_t buf[EVBUF];
    volatile int head, tail;
    volatile uint32_t count;
    proc_t *waiter;
    spinlock_irqsave_t lock;
    uint32_t dropped;
    uint32_t syn_dropped;
    uint64_t reads;
    uint64_t syncs;
    uint64_t last_read;
    uint8_t stall_logged;
    uint32_t rd_pid;
    char rd_name[32];
    uint32_t wd_pid;
    uint8_t wd_logged;
    uint64_t wd_last_remind;
    uint64_t last_drop_log;
    uint64_t last_flush_log;
    uint64_t last_sync_log;
} evdev_t;

static evdev_t g_evdev[INPUT_NDEVS];
int g_evdev_kbd_open = 0;

/* Rate-limit noisy diagnostics to one message per `ms` (g_ticks is ms). */
static inline bool evdev_log_gate(uint64_t *last, uint64_t ms) {
    uint64_t now = g_ticks;
    if (now - *last < ms) return false;
    *last = now;
    return true;
}

void input_push(int dev, uint16_t type, uint16_t code, int32_t value) {
    if ((unsigned) dev >= INPUT_NDEVS) return;
    evdev_t *e = &g_evdev[dev];
    spin_lock_irqsave(&e->lock);
    if (e->count >= EVBUF) {
        if (!e->stall_logged && e->last_read && (int64_t) (g_ticks - e->last_read) > 200) {
            e->stall_logged = 1;
            log_warn("INPUT: evdev dev=%d reader stalled (%llu ms since last read, count=%u) "
                     "last reader=%s(pid=%u)",
                     dev, (unsigned long long) (g_ticks - e->last_read), e->count, e->rd_name,
                     e->rd_pid);
        }
        e->tail = (e->tail + 1) % EVBUF;
        e->count--;
        e->dropped++;
        e->syn_dropped = 1;
        if (evdev_log_gate(&e->last_drop_log, 1000))
            log_warn("INPUT: evdev dev=%d dropping oldest (%u dropped so far in burst)", dev,
                     e->dropped);
    }
    /* On overflow the next SYN carries SYN_DROPPED so libinput resyncs. */
    if (type == EV_SYN && e->syn_dropped) {
        code = SYN_DROPPED;
        e->syn_dropped = 0;
    }
    e->buf[e->head] = (input_event_t) { .sec = g_ticks / 1000,
                                        .usec = (g_ticks % 1000) * 1000,
                                        .type = type,
                                        .code = code,
                                        .value = value };
    e->head = (e->head + 1) % EVBUF;
    e->count++;
    bool notify = e->count == 1;
    proc_t *w = e->waiter;
    if (w && __sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY))
        proc_set_ready(w);
    e->waiter = NULL;
    spin_unlock_irqrestore(&e->lock);
    if (notify) poll_notify();
}

static void kbd_evdev_push(uint16_t key, int value) {
    input_push(INPUT_DEV_KBD, EV_KEY, key, value);
    input_push(INPUT_DEV_KBD, EV_SYN, SYN_REPORT, 0);
}

/* Timer-IRQ watchdog: log when a reader stalls (once per episode, then every
 * WD_REMIND_TICKS). IRQ context: lock-free/IRQ-safe only; never kills. */
static const char *walk_syscall_name(int64_t nr) {
    switch (nr) {
    case 0: return "read";
    case 1: return "write";
    case 2: return "open";
    case 3: return "close";
    case 16: return "ioctl";
    case 23: return "nanosleep";
    case 34: return "mkdir";
    case 35: return "unlink";
    case 39: return "getpid";
    case 41: return "socket";
    case 42: return "connect";
    case 43: return "accept";
    case 44: return "sendmsg";
    case 45: return "recvfrom";
    case 46: return "sendto";
    case 47: return "recvmsg";
    case 48: return "shutdown";
    case 49: return "bind";
    case 50: return "listen";
    case 51: return "getsockname";
    case 52: return "getpeername";
    case 53: return "socketpair";
    case 54: return "setsockopt";
    case 55: return "getsockopt";
    case 56: return "clone";
    case 57: return "fork";
    case 60: return "exit";
    case 62: return "kill";
    case 63: return "uname";
    case 72: return "fcntl";
    case 80: return "fstat";
    case 90: return "mmap";
    case 91: return "munmap";
    case 93: return "futex";
    case 191: return "getrlimit";
    case 202: return "futex_time64";
    case 213: return "mmap2";
    case 218: return "mincore";
    case 288: return "accept4";
    default: return "?";
    }
}

void input_watchdog(void) {
    for (int dev = 0; dev < INPUT_NDEVS; dev++) {
        evdev_t *e = &g_evdev[dev];
        if (!e->stall_logged || !e->rd_pid) continue;

        proc_t *p = proc_slot_of_pid(e->rd_pid);
        if (!p) continue; /* reader already gone; chr_close will re-arm the tty */

        int64_t unread = (int64_t) (g_ticks - e->last_read);
        if (unread < 0) unread = 0;

        if (e->wd_pid != e->rd_pid) {
            e->wd_pid = e->rd_pid;
            e->wd_logged = 0;
            e->wd_last_remind = 0;
        }
        if (!e->wd_logged) {
            e->wd_logged = 1;
            int st = __atomic_load_n(&p->state, __ATOMIC_RELAXED);
            int64_t cs = __atomic_load_n(&p->cur_syscall, __ATOMIC_RELAXED);
            int64_t arg0 = __atomic_load_n(&p->cur_syscall_arg0, __ATOMIC_RELAXED);
            log_warn("INPUT: wedged evdev dev=%d reader %s(pid=%u) state=%d cur_syscall=%ld(%s,arg0=%ld) "
                     "reads=%llu dropped=%u count=%u unread=%llu ms",
                     dev, e->rd_name, e->rd_pid, st, (long) cs, walk_syscall_name(cs), (long) arg0,
                     (unsigned long long) e->reads, e->dropped, e->count,
                     (unsigned long long) unread);
        }
        if ((uint64_t) unread > WD_GRACE_TICKS &&
            (e->wd_last_remind == 0 ||
             (int64_t) (g_ticks - e->wd_last_remind) >= (int64_t) WD_REMIND_TICKS)) {
            e->wd_last_remind = g_ticks;
            log_warn("INPUT: evdev dev=%d still wedged, reader=%s(pid=%u) unread %llu ms "
                     "(diagnostic only, no kill)",
                     dev, e->rd_name, e->rd_pid, (unsigned long long) unread);
        }
    }
}

static bool evdev_pollin(vfs_node_t *n) {
    int dev = (int) (uintptr_t) n->data;
    if ((unsigned) dev >= INPUT_NDEVS) return false;
    evdev_t *e = &g_evdev[dev];
    spin_lock_irqsave(&e->lock);
    bool ready = e->count > 0;
    spin_unlock_irqrestore(&e->lock);
    return ready;
}

static int64_t evdev_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) off;
    int dev = (int) (uintptr_t) n->data;
    if ((unsigned) dev >= INPUT_NDEVS || len < sizeof(input_event_t)) return -EINVAL;

    evdev_t *e = &g_evdev[dev];
    uint8_t *out = (uint8_t *) buf;
    uint64_t written = 0;
    for (;;) {
        spin_lock_irqsave(&e->lock);
        while (e->count > 0 && written + sizeof(input_event_t) <= len) {
            input_event_t ev;
            __builtin_memcpy(&ev, &e->buf[e->tail], sizeof(input_event_t));
            if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
                e->syncs++;
                if (evdev_log_gate(&e->last_sync_log, 1000))
                    log_warn("INPUT: evdev dev=%d reader consumed SYN_DROPPED (reads=%llu syncs=%llu)",
                             dev, (unsigned long long) e->reads, (unsigned long long) e->syncs);
            }
            __builtin_memcpy(out + written, &ev, sizeof(input_event_t));
            e->tail = (e->tail + 1) % EVBUF;
            e->count--;
            written += sizeof(input_event_t);
        }
        if (written > 0 || e->count > 0 || len < sizeof(input_event_t)) {
            if (e->dropped && written > 0) {
                if (evdev_log_gate(&e->last_flush_log, 1000))
                    log_warn("INPUT: evdev dev=%d flushed %lu events after %u dropped", dev,
                             written, e->dropped);
                e->dropped = 0;
            }
            if (written > 0) {
                proc_t *rp = g_current_proc;
                if (rp) {
                    e->rd_pid = rp->pid;
                    const char *s = rp->exe_path[0] ? rp->exe_path : "?";
                    const char *base = s;
                    for (const char *c = s; *c; c++)
                        if (*c == '/') base = c + 1;
                    size_t n = strlen(base);
                    if (n >= sizeof(e->rd_name)) n = sizeof(e->rd_name) - 1;
                    memcpy(e->rd_name, base, n);
                    e->rd_name[n] = 0;
                }
                e->reads++;
                e->last_read = g_ticks;
                e->stall_logged = 0;
                e->wd_pid = 0;
                e->wd_logged = 0;
                e->wd_last_remind = 0;
                if ((e->reads & 0x3FF) == 1)
                    log_debug("INPUT: evdev dev=%d reader alive (reads=%llu) %s(pid=%u)", dev,
                              (unsigned long long) e->reads, e->rd_name, e->rd_pid);
            }
            spin_unlock_irqrestore(&e->lock);
            break;
        }
        proc_t *p = g_current_proc;
        e->waiter = p;
        if (p) p->state = PROC_WAITING;
        spin_unlock_irqrestore(&e->lock);
        if (!p) break;
        sched_block_current();
        e->waiter = NULL;
        /* A pending unmasked signal must escape the blocking read: check once
         * we are woken, otherwise signal_check never runs (it only runs on
         * syscall exit) and the reader sleeps forever on an empty buffer. */
        if (p) {
            uint64_t pending = __atomic_load_n(&p->pending_sigs, __ATOMIC_RELAXED);
            if (pending & ~p->sig_mask) return -(int64_t) EINTR;
        }
    }
    return (int64_t) written;
}

// _IOC(READ=2, 'E', nr, size) = (2<<30)|(size<<16)|('E'<<8)|nr
#define EVIO(nr, sz) ((2u << 30) | ((sz) << 16) | (0x45u << 8) | (nr))
#define EVIOCGVERSION EVIO(0x01, 4)
#define EVIOCGID EVIO(0x02, 8)
#define EVIOCGPROP(n) EVIO(0x09, (n))
#define EVIOCGKEY(n) EVIO(0x18, (n))

static bool evio_req(uint64_t req, uint8_t nr) {
    return (req >> 30) == 2 && ((req >> 8) & 0xFF) == 0x45 && (req & 0xFF) == nr;
}

static int64_t evdev_ioctl(vfs_node_t *n, uint64_t req64, uint64_t arg) {
    int dev = (int) (uintptr_t) n->data;
    uint64_t req = (uint32_t) req64;

    // name: EVIO(0x06, len)
    if (evio_req(req, 0x06)) {
        uint32_t len = (uint32_t) ((req >> 16) & 0x3FFF);
        const char *name = dev == INPUT_DEV_KBD ? "Kyronix Keyboard" : "Kyronix Mouse";
        uint32_t n2 = (uint32_t) strlen(name) + 1;
        if (n2 > len) n2 = len;
        if (n2 && !uptr_ok_w((void *) (uintptr_t) arg, n2)) return -14;
        __builtin_memcpy((void *) (uintptr_t) arg, name, n2);
        return (int64_t) n2;
    }

    if (evio_req(req, 0x09) || evio_req(req, 0x18)) {
        uint32_t len = (uint32_t) ((req >> 16) & 0x3FFF);
        if (arg && len) {
            if (!uptr_ok_w((void *) (uintptr_t) arg, len)) return -14;
            __builtin_memset((void *) (uintptr_t) arg, 0, len);
        }
        return 0;
    }

    // EVIOCGBIT(ev_type, len): dir=read, type='E', nr in [0x20,0x3F]
    if ((req >> 30) == 2 && ((req >> 8) & 0xFF) == 0x45 && (req & 0xFF) >= 0x20 &&
        (req & 0xFF) < 0x40) {
        uint32_t ev_type = (uint32_t) (req & 0xFF) - 0x20;
        uint32_t len = (uint32_t) ((req >> 16) & 0x3FFF);
        uint8_t *bits = (uint8_t *) (uintptr_t) arg;
        if (!bits || !len) return 0;
        if (!uptr_ok_w(bits, len)) return -14;
        __builtin_memset(bits, 0, len);
        if (ev_type == 0) { // supported event types
            if (dev == INPUT_DEV_KBD) {
                bits[0] |= (1 << EV_SYN) | (1 << EV_KEY); // EV_REP=0x14
                if (2 < len) bits[2] |= (1 << (0x14 - 16));
            } else {
                bits[0] |= (1 << EV_SYN) | (1 << EV_KEY) | (1 << EV_REL);
            }
        } else if (ev_type == EV_KEY && dev == INPUT_DEV_KBD) {
            // set bits for keys 1-127
            for (int i = 1; i <= 127 && i / 8 < (int) len; i++)
                bits[i / 8] |= (uint8_t) (1u << (i % 8));
        } else if (ev_type == EV_KEY && dev == INPUT_DEV_MOUSE) {
            // BTN_LEFT=0x110, RIGHT=0x111, MIDDLE=0x112
            if (0x110 / 8 < (int) len) bits[0x110 / 8] |= 0x07 << (0x110 % 8);
        } else if (ev_type == EV_REL && dev == INPUT_DEV_MOUSE) {
            if (0 < (int) len) bits[0] |= (1 << REL_X) | (1 << REL_Y);
            if (1 < (int) len) bits[1] |= (1 << (REL_WHEEL - 8));
        }
        return 0;
    }

    switch (req) {
    case EVIOCGVERSION:
        if (!arg || !uptr_ok_w((void *) (uintptr_t) arg, sizeof(uint32_t))) return -14;
        *(uint32_t *) (uintptr_t) arg = 0x010001;
        return 0;
    case EVIOCGID:
        if (!arg || !uptr_ok_w((void *) (uintptr_t) arg, 4 * sizeof(uint16_t))) return -14;
        ((uint16_t *) (uintptr_t) arg)[0] = 0x11; // BUS_I8042
        ((uint16_t *) (uintptr_t) arg)[1] = 1;
        ((uint16_t *) (uintptr_t) arg)[2] = dev == INPUT_DEV_KBD ? 1 : 2;
        ((uint16_t *) (uintptr_t) arg)[3] = 1;
        return 0;
    default:
        return 0; // ignore unkniwn evio ioctls
    }
}

static int evdev_open(vfs_node_t *n, int flags) {
    int dev = (int) (uintptr_t) n->data;
    if ((unsigned) dev < INPUT_NDEVS && dev == INPUT_DEV_KBD)
        __atomic_add_fetch(&g_evdev_kbd_open, 1, __ATOMIC_RELAXED);
    int fd = fd_open_node(n, flags);
    if (fd < 0 && (unsigned) dev < INPUT_NDEVS && dev == INPUT_DEV_KBD)
        __atomic_sub_fetch(&g_evdev_kbd_open, 1, __ATOMIC_RELAXED);
    return fd;
}

static void evdev_close(vfs_node_t *n) {
    int dev = (int) (uintptr_t) n->data;
    if ((unsigned) dev < INPUT_NDEVS && dev == INPUT_DEV_KBD)
        __atomic_sub_fetch(&g_evdev_kbd_open, 1, __ATOMIC_RELAXED);
}

void input_init(void) {
    for (int i = 0; i < INPUT_NDEVS; i++) g_evdev[i].lock.lock.lock = 0;
    vfs_node_t *kbd_node = vfs_create_chr("/dev/input/event0", evdev_read, NULL);
    vfs_node_t *mouse_node = vfs_create_chr("/dev/input/event1", evdev_read, NULL);

    if (kbd_node) {
        kbd_node->mode = S_IFCHR | 0600;
        kbd_node->data = (uint8_t *) (uintptr_t) INPUT_DEV_KBD;
        kbd_node->chr_ioctl = evdev_ioctl;
        kbd_node->chr_pollin = evdev_pollin;
        kbd_node->chr_open = evdev_open;
        kbd_node->chr_close = evdev_close;
        kbd_node->rdev = VFS_MKDEV(13, 64); // INPUT_MAJOR:event0
    }
    if (mouse_node) {
        mouse_node->mode = S_IFCHR | 0600;
        mouse_node->data = (uint8_t *) (uintptr_t) INPUT_DEV_MOUSE;
        mouse_node->chr_ioctl = evdev_ioctl;
        mouse_node->chr_pollin = evdev_pollin;
        mouse_node->chr_open = evdev_open;
        mouse_node->rdev = VFS_MKDEV(13, 65); // event1
    }

    g_kbd_evdev_hook = kbd_evdev_push;
}
