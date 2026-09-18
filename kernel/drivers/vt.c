#include "vt.h"
#include "../fs/vfs.h"
#include "../fs/vfs_internal.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../syscall/syscall.h"
#include "fb.h"
#include "tty.h"

#define KDSETMODE 0x4B3A
#define KDGETMODE 0x4B3B
#define KDGKBMODE 0x4B44
#define KDSKBMODE 0x4B45
#define KDGKBTYPE 0x4B33
#define KDMAPDISP 0x4B3C
#define KDUNMAPDISP 0x4B3D
#define GIO_SCRNMAP 0x4B20
#define PIO_SCRNMAP 0x4B21
#define GIO_CMAP 0x4B70
#define PIO_CMAP 0x4B71

#define VT_OPENQRY 0x5600
#define VT_GETMODE 0x5601
#define VT_SETMODE 0x5602
#define VT_GETSTATE 0x5603
#define VT_SENDSIG 0x5604
#define VT_RELDISP 0x5605
#define VT_ACTIVATE 0x5606
#define VT_WAITACTIVE 0x5607
#define VT_DISALLOCATE 0x5608
#define VT_SETSCRNMAP 0x560A

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TCGETA 0x5405
#define TCSETA 0x5406
#define TIOCSCTTY 0x540B
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCNOTTY 0x5422
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define FIONREAD 0x541B
#define TIOCEXCL 0x540C
#define TIOCNXCL 0x540D

#define KD_TEXT 0
#define KD_GRAPHICS 1
#define K_RAW 0x00
#define K_XLATE 0x01
#define K_MEDIUMRAW 0x02
#define K_UNICODE 0x03
#define KB_101 0x02

#define VT_AUTO 0
#define VT_PROCESS 1

typedef struct {
    char mode, waitv;
    short relsig, acqsig, frsig;
} vt_mode_t;
typedef struct {
    unsigned short v_active, v_signal, v_state;
} vt_stat_t;
typedef struct {
    unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
} winsize_t;

#define NCCS_VT 19
typedef struct {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_cc[NCCS_VT];
} vt_termios_t;

static int g_kd_mode = KD_TEXT;
static int g_kb_mode = K_XLATE;

static int64_t vt_ioctl(vfs_node_t *n, uint64_t req, uint64_t arg) {
    (void) n;
    switch (req) {
    case KDGETMODE:
        if (arg && !uptr_ok_w((void *) (uintptr_t) arg, sizeof(int))) return -14;
        if (arg) *(int *) (uintptr_t) arg = g_kd_mode;
        return 0;
    case KDSETMODE:
        g_kd_mode = (int) arg;
        return 0;
    case KDGKBMODE:
        if (arg && !uptr_ok_w((void *) (uintptr_t) arg, sizeof(int))) return -14;
        if (arg) *(int *) (uintptr_t) arg = g_kb_mode;
        return 0;
    case KDSKBMODE:
        g_kb_mode = (int) arg;
        return 0;
    case KDGKBTYPE:
        if (arg && !uptr_ok_w((void *) (uintptr_t) arg, sizeof(char))) return -14;
        if (arg) *(char *) (uintptr_t) arg = (char) KB_101;
        return 0;
    case KDMAPDISP:
    case KDUNMAPDISP:
    case GIO_SCRNMAP:
    case PIO_SCRNMAP:
    case GIO_CMAP:
    case PIO_CMAP:
        return 0;

    case VT_OPENQRY:
        if (arg && !uptr_ok_w((void *) (uintptr_t) arg, sizeof(int))) return -14;
        if (arg) *(int *) (uintptr_t) arg = 1; /* VT 1 is free */
        return 0;
    case VT_GETSTATE: {
        vt_stat_t *s = (vt_stat_t *) (uintptr_t) arg;
        if (!s) return -22;
        if (!uptr_ok_w(s, sizeof(*s))) return -14;
        s->v_active = 1;
        s->v_signal = 0;
        s->v_state = 0x0002; /* bit 1 = VT1 open */
        return 0;
    }
    case VT_GETMODE: {
        vt_mode_t *m = (vt_mode_t *) (uintptr_t) arg;
        if (!m) return -22;
        if (!uptr_ok_w(m, sizeof(*m))) return -14;
        m->mode = VT_AUTO;
        m->waitv = 0;
        m->relsig = m->acqsig = m->frsig = 0;
        return 0;
    }
    case VT_SETMODE:
    case VT_RELDISP:
    case VT_ACTIVATE:
    case VT_WAITACTIVE:
    case VT_DISALLOCATE:
    case VT_SETSCRNMAP:
        return 0;

    case TIOCGWINSZ: {
        winsize_t *ws = (winsize_t *) (uintptr_t) arg;
        if (!ws) return -22;
        if (!uptr_ok_w(ws, sizeof(*ws))) return -14;
        ws->ws_row = (unsigned short) (g_fb.height / FONT_H);
        ws->ws_col = (unsigned short) (g_fb.width / FONT_W);
        ws->ws_xpixel = (unsigned short) g_fb.width;
        ws->ws_ypixel = (unsigned short) g_fb.height;
        return 0;
    }
    case TIOCSWINSZ:
        return 0;

    case TCGETS: {
        vt_termios_t *t = (vt_termios_t *) (uintptr_t) arg;
        if (!t) return -22;
        if (!uptr_ok_w(t, sizeof(*t))) return -14;
        struct termios_s ts;
        tty_get_termios(&ts);
        t->c_iflag = ts.c_iflag;
        t->c_oflag = ts.c_oflag;
        t->c_cflag = ts.c_cflag;
        t->c_lflag = ts.c_lflag;
        __builtin_memcpy(t->c_cc, ts.c_cc, NCCS_VT < NCCS ? NCCS_VT : NCCS);
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        vt_termios_t *t = (vt_termios_t *) (uintptr_t) arg;
        if (!t) return 0;
        if (!uptr_ok(t, sizeof(*t))) return -14;
        struct termios_s ts;
        ts.c_iflag = t->c_iflag;
        ts.c_oflag = t->c_oflag;
        ts.c_cflag = t->c_cflag;
        ts.c_lflag = t->c_lflag;
        __builtin_memcpy(ts.c_cc, t->c_cc, NCCS_VT < NCCS ? NCCS_VT : NCCS);
        tty_set_termios(&ts);
        return 0;
    }
    case TCGETA:
    case TCSETA:
        return 0;

    case TIOCGPGRP:
        if (arg && !uptr_ok_w((void *) (uintptr_t) arg, sizeof(int))) return -14;
        if (arg) *(int *) (uintptr_t) arg = tty_get_fg_pgid();
        return 0;
    case TIOCSPGRP:
        if (arg && !uptr_ok((void *) (uintptr_t) arg, sizeof(int))) return -14;
        if (arg) tty_set_fg_pgid(*(int *) (uintptr_t) arg);
        return 0;
    case TIOCSCTTY:
    case TIOCNOTTY:
    case TIOCEXCL:
    case TIOCNXCL:
        return 0;

    case FIONREAD:
        if (arg && !uptr_ok_w((void *) (uintptr_t) arg, sizeof(int))) return -14;
        if (arg) *(int *) (uintptr_t) arg = 0;
        return 0;

    default:
        return -25; /* ENOTTY */
    }
}

static int64_t vt_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    (void) buf;
    (void) len;
    (void) off;
    return 0;
}

static int64_t vt_write(vfs_node_t *n, const char *buf, uint64_t len, uint64_t pos) {
    (void) n;
    (void) buf;
    return (int64_t) len;
}

void vt_init(void) {
    char path[16];
    for (int i = 0; i <= 7; i++) {
        snprintf(path, sizeof(path), "/dev/tty%d", i);
        vfs_node_t *n = vfs_create_chr(path, vt_read, vt_write);
        if (n) n->chr_ioctl = vt_ioctl;
    }

    /* replace /dev/console symlink with proper vt chr dev */
    vfs_unlink("/dev/console");
    vfs_node_t *nc = vfs_create_chr("/dev/console", vt_read, vt_write);
    if (nc) nc->chr_ioctl = vt_ioctl;

    /* attach vt_ioctl to /dev/tty so VT/KD ioctls work on the controlling terminal too */
    vfs_node_t *tty = vfs_lookup("/dev/tty");
    if (tty) {
        tty->chr_ioctl = vt_ioctl;
        vfs_node_unref_internal(tty);
    }
}
