#include "input.h"
#include "../arch/x86_64/pit.h"
#include "../fs/vfs.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../proc/proc.h"
#include "../syscall/syscall.h"
#include "kbd.h"
#include <stdbool.h>

#define EINVAL 22
#define ENOSYS 38

#define EVBUF 128
typedef struct
{
    input_event_t buf[EVBUF];
    volatile int head, tail;
    proc_t* waiter;
} evdev_t;

static evdev_t g_evdev[INPUT_NDEVS];
int g_evdev_kbd_open = 0;

void input_push(int dev, uint16_t type, uint16_t code, int32_t value)
{
    if ((unsigned) dev >= INPUT_NDEVS)
        return;
    evdev_t* e = &g_evdev[dev];
    int next = (e->head + 1) % EVBUF;
    if (next == e->tail)
        return; /* full, drop */
    e->buf[e->head] = (input_event_t) {.sec = g_ticks / 1000,
                                       .usec = (g_ticks % 1000) * 1000,
                                       .type = type,
                                       .code = code,
                                       .value = value};
    e->head = next;
    if (e->waiter && e->waiter->state == PROC_WAITING)
        e->waiter->state = PROC_READY;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proctable[i].state == PROC_WAITING)
            g_proctable[i].state = PROC_READY;
}

static void kbd_evdev_push(uint16_t key, int value)
{
    input_push(INPUT_DEV_KBD, EV_KEY, key, value);
    input_push(INPUT_DEV_KBD, EV_SYN, 0, 0);
}

static bool evdev_pollin(vfs_node_t* n)
{
    int dev = (int) (uintptr_t) n->data;
    if ((unsigned) dev >= INPUT_NDEVS)
        return false;
    return g_evdev[dev].head != g_evdev[dev].tail;
}

static int64_t evdev_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) off;
    int dev = (int) (uintptr_t) n->data;
    if ((unsigned) dev >= INPUT_NDEVS || len < sizeof(input_event_t))
        return -EINVAL;
    if (dev == INPUT_DEV_KBD)
        g_evdev_kbd_open = 1; /* X grabbed kbd — mute tty echo */

    evdev_t* e = &g_evdev[dev];
    e->waiter = g_current_proc;
    while (e->head == e->tail)
    {
        if (g_current_proc)
            g_current_proc->wakeup_tick = g_ticks + 10;
        sched_yield_blocking();
        if (g_current_proc)
            g_current_proc->wakeup_tick = 0;
    }
    e->waiter = NULL;

    uint64_t written = 0;
    while (e->head != e->tail && written + sizeof(input_event_t) <= len)
    {
        __builtin_memcpy(buf + written, &e->buf[e->tail], sizeof(input_event_t));
        e->tail = (e->tail + 1) % EVBUF;
        written += sizeof(input_event_t);
    }
    return (int64_t) written;
}

/* _IOC(READ=2, 'E', nr, size) = (2<<30)|(size<<16)|('E'<<8)|nr */
#define EVIO(nr, sz) ((2u << 30) | ((sz) << 16) | (0x45u << 8) | (nr))
#define EVIOCGVERSION EVIO(0x01, 4)
#define EVIOCGID EVIO(0x02, 8)
#define EVIOCGPROP(n) EVIO(0x09, (n))
#define EVIOCGKEY(n) EVIO(0x18, (n))

static bool evio_req(uint64_t req, uint8_t nr)
{
    return (req >> 30) == 2 && ((req >> 8) & 0xFF) == 0x45 && (req & 0xFF) == nr;
}

static int64_t evdev_ioctl(vfs_node_t* n, uint64_t req64, uint64_t arg)
{
    int dev = (int) (uintptr_t) n->data;
    /* ioctl request is a 32-bit int; userspace sign-extends it (bit 31 set on
       _IOC_READ requests), so the high half arrives as 0xffffffff — truncate it */
    uint64_t req = (uint32_t) req64;

    /* name: EVIO(0x06, len) */
    if (evio_req(req, 0x06))
    {
        uint32_t len = (uint32_t) ((req >> 16) & 0x3FFF);
        const char* name = dev == INPUT_DEV_KBD ? "Kyronix Keyboard" : "Kyronix Mouse";
        uint32_t n2 = (uint32_t) strlen(name) + 1;
        if (n2 > len)
            n2 = len;
        if (n2 && !uptr_ok_w((void*) (uintptr_t) arg, n2))
            return -14;
        __builtin_memcpy((void*) (uintptr_t) arg, name, n2);
        return (int64_t) n2;
    }

    if (evio_req(req, 0x09) || evio_req(req, 0x18))
    {
        uint32_t len = (uint32_t) ((req >> 16) & 0x3FFF);
        if (arg && len)
        {
            if (!uptr_ok_w((void*) (uintptr_t) arg, len))
                return -14;
            __builtin_memset((void*) (uintptr_t) arg, 0, len);
        }
        return 0;
    }

    /* EVIOCGBIT(ev_type, len): dir=read, type='E', nr in [0x20,0x3F] */
    if ((req >> 30) == 2 && ((req >> 8) & 0xFF) == 0x45 && (req & 0xFF) >= 0x20 &&
        (req & 0xFF) < 0x40)
    {
        uint32_t ev_type = (uint32_t) (req & 0xFF) - 0x20;
        uint32_t len = (uint32_t) ((req >> 16) & 0x3FFF);
        uint8_t* bits = (uint8_t*) (uintptr_t) arg;
        if (!bits || !len)
            return 0;
        if (!uptr_ok_w(bits, len))
            return -14;
        __builtin_memset(bits, 0, len);
        if (ev_type == 0)
        { /* supported event types */
            if (dev == INPUT_DEV_KBD)
            {
                bits[0] |= (1 << EV_SYN) | (1 << EV_KEY); /* EV_REP=0x14 */
                if (2 < len)
                    bits[2] |= (1 << (0x14 - 16));
            }
            else
            {
                bits[0] |= (1 << EV_SYN) | (1 << EV_KEY) | (1 << EV_REL);
            }
        }
        else if (ev_type == EV_KEY && dev == INPUT_DEV_KBD)
        {
            /* set bits for keys 1-127 */
            for (int i = 1; i <= 127 && i / 8 < (int) len; i++)
                bits[i / 8] |= (uint8_t) (1u << (i % 8));
        }
        else if (ev_type == EV_KEY && dev == INPUT_DEV_MOUSE)
        {
            /* BTN_LEFT=0x110, RIGHT=0x111, MIDDLE=0x112 */
            if (0x110 / 8 < (int) len)
                bits[0x110 / 8] |= 0x07 << (0x110 % 8);
        }
        else if (ev_type == EV_REL && dev == INPUT_DEV_MOUSE)
        {
            if (0 < (int) len)
                bits[0] |= (1 << REL_X) | (1 << REL_Y);
            if (1 < (int) len)
                bits[1] |= (1 << (REL_WHEEL - 8));
        }
        return 0;
    }

    switch (req)
    {
    case EVIOCGVERSION:
        if (!arg || !uptr_ok_w((void*) (uintptr_t) arg, sizeof(uint32_t)))
            return -14;
        *(uint32_t*) (uintptr_t) arg = 0x010001;
        return 0;
    case EVIOCGID:
        if (!arg || !uptr_ok_w((void*) (uintptr_t) arg, 4 * sizeof(uint16_t)))
            return -14;
        ((uint16_t*) (uintptr_t) arg)[0] = 0x11; /* BUS_I8042 */
        ((uint16_t*) (uintptr_t) arg)[1] = 1;
        ((uint16_t*) (uintptr_t) arg)[2] = dev == INPUT_DEV_KBD ? 1 : 2;
        ((uint16_t*) (uintptr_t) arg)[3] = 1;
        return 0;
    default:
        return 0; /* ignore unknown evio ioctls */
    }
}

void input_init(void)
{
    vfs_node_t* kbd_node = vfs_create_chr("/dev/input/event0", evdev_read, NULL);
    vfs_node_t* mouse_node = vfs_create_chr("/dev/input/event1", evdev_read, NULL);

    if (kbd_node)
    {
        kbd_node->data = (uint8_t*) (uintptr_t) INPUT_DEV_KBD;
        kbd_node->chr_ioctl = evdev_ioctl;
        kbd_node->chr_pollin = evdev_pollin;
    }
    if (mouse_node)
    {
        mouse_node->data = (uint8_t*) (uintptr_t) INPUT_DEV_MOUSE;
        mouse_node->chr_ioctl = evdev_ioctl;
        mouse_node->chr_pollin = evdev_pollin;
    }

    g_kbd_evdev_hook = kbd_evdev_push;
}
