#include "devfs.h"
#include "crypto/chacha20.h"
#include "drivers/tty.h"
#include "fs/pipe.h"
#include "fs/vfs.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/vmm.h"
#include "proc/proc.h"
#include "syscall/syscall.h"

#define EFAULT 14
#define EINVAL 22
#define EPERM 1

static int64_t devmem_mmap(vfs_node_t *n, uint64_t off, uint64_t len, uint64_t va,
                           uint64_t vflags) {
    (void) n;
    proc_t *p = g_current_proc;
    if (!p || !p->space) return -EINVAL;
    if (p->euid != 0) return -(int64_t) EPERM;
    off &= ~0xFFFULL;
    if (va >= USER_LIMIT || len > USER_LIMIT - va) return -EINVAL;
    if (off + len < off) return -EINVAL;
    uint64_t flags = vflags | VMM_PRESENT | VMM_USER | VMM_WRITE;
    for (uint64_t o = 0; o < len; o += 0x1000) vmm_map(p->space, va + o, off + o, flags);
    return (int64_t) va;
}

static int64_t dev_null_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    (void) buf;
    (void) len;
    (void) off;
    return 0;
}

static int64_t dev_null_write(vfs_node_t *n, const char *buf, uint64_t len, uint64_t pos) {
    (void) n;
    (void) buf;
    return (int64_t) len;
}

static int64_t dev_zero_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    (void) off;
    memset(buf, 0, len);
    return (int64_t) len;
}

static int64_t dev_urandom_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    (void) off;
    chacha20_rng_bytes(&g_chacha20_rng, (uint8_t *) buf, (size_t) len);
    return (int64_t) len;
}

// add entropy when someone weite to /dev/random
static int64_t dev_random_write(vfs_node_t *n, const char *buf, uint64_t len, uint64_t pos) {
    (void) n;
    chacha20_rng_mix(&g_chacha20_rng, (const uint8_t *) buf, (size_t) len);
    return (int64_t) len;
}

static int64_t dev_tty_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    (void) off;
    return tty_read(buf, len);
}

static int64_t dev_tty_write(vfs_node_t *n, const char *buf, uint64_t len, uint64_t pos) {
    (void) n;
    return tty_write(buf, len);
}

#define PTY_MAX 16
#define ENOSPC 28
#define ENOMEM 12

#define PTY_NCCS 19

/* Userspace termios/winsize as the Linux ioctls carry them */
typedef struct {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_cc[PTY_NCCS];
} pty_termios_t;

typedef struct {
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
} pty_winsize_t;

typedef struct {
    pipe_t *m2s;
    pipe_t *s2m;
    int num;
    int used;
    int pgid;
    pty_termios_t termios;
    pty_winsize_t ws;
    vfs_node_t master_node; // not in vfs tree
    vfs_node_t *slave;
} pty_inst_t;

static pty_inst_t g_ptys[PTY_MAX];

static pty_inst_t *pty_of(vfs_node_t *n) {
    int idx = (int) (n->size & 0xFF);
    if (idx < 0 || idx >= PTY_MAX || !g_ptys[idx].used) return NULL;
    return &g_ptys[idx];
}

// master r/w/ioctl/close
static int64_t ptym_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) off;
    pty_inst_t *p = pty_of(n);
    return p ? pipe_read(p->s2m, buf, len) : -(int64_t) EINVAL;
}

static int64_t ptym_write(vfs_node_t *n, const char *buf, uint64_t len, uint64_t pos) {
    pty_inst_t *p = pty_of(n);
    return p ? pipe_write(p->m2s, buf, len) : -(int64_t) EINVAL;
}

static bool ptym_pollin(vfs_node_t *n) {
    pty_inst_t *p = pty_of(n);
    return p && p->s2m->count > 0;
}

/* Terminal ioctls both ends of a pty answer. weston-terminal needs at least
   TIOCSCTTY (through login_tty) and TIOCSWINSZ; the shell inside needs the
   termios pair. */
static int64_t pty_common_ioctl(pty_inst_t *p, uint64_t req, uint64_t arg) {
    switch ((uint32_t) req) {
    case 0x5401: { /* TCGETS */
        if (!p || !uptr_ok_w((void *) (uintptr_t) arg, sizeof(pty_termios_t))) return -EFAULT;
        memcpy((void *) (uintptr_t) arg, &p->termios, sizeof(p->termios));
        return 0;
    }
    case 0x5402: /* TCSETS */
    case 0x5403: /* TCSETSW */
    case 0x5404: { /* TCSETSF */
        if (!p || !uptr_ok((void *) (uintptr_t) arg, sizeof(pty_termios_t))) return -EFAULT;
        memcpy(&p->termios, (const void *) (uintptr_t) arg, sizeof(p->termios));
        return 0;
    }
    case 0x5413: { /* TIOCGWINSZ */
        if (!p || !uptr_ok_w((void *) (uintptr_t) arg, sizeof(pty_winsize_t))) return -EFAULT;
        memcpy((void *) (uintptr_t) arg, &p->ws, sizeof(p->ws));
        return 0;
    }
    case 0x5414: { /* TIOCSWINSZ */
        if (!p || !uptr_ok((void *) (uintptr_t) arg, sizeof(pty_winsize_t))) return -EFAULT;
        memcpy(&p->ws, (const void *) (uintptr_t) arg, sizeof(p->ws));
        return 0;
    }
    case 0x540F: /* TIOCGPGRP */
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(int))) return -EFAULT;
        *(int *) (uintptr_t) arg = p ? p->pgid : 0;
        return 0;
    case 0x5410: /* TIOCSPGRP */
        if (!uptr_ok((void *) (uintptr_t) arg, sizeof(int))) return -EFAULT;
        if (p) p->pgid = *(const int *) (uintptr_t) arg;
        return 0;
    case 0x540E: /* TIOCSCTTY */
    case 0x5422: /* TIOCNOTTY */
    case 0x540C: /* TIOCEXCL */
    case 0x540D: /* TIOCNXCL */
    case 0x5409: /* TCSBRK */
    case 0x540A: /* TCXONC */
    case 0x540B: /* TCFLSH */
    case 0x5405: /* TCGETA */
    case 0x5406: /* TCSETA */
        return 0;
    default:
        return -EINVAL;
    }
}

static int64_t ptys_ioctl(vfs_node_t *n, uint64_t req, uint64_t arg) {
    pty_inst_t *p = pty_of(n);
    if ((uint32_t) req == 0x541B) { /* FIONREAD */
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(int))) return -EFAULT;
        *(int *) (uintptr_t) arg = (int) ((p && p->m2s) ? p->m2s->count : 0);
        return 0;
    }
    return pty_common_ioctl(p, req, arg);
}

static int64_t ptym_ioctl(vfs_node_t *n, uint64_t req, uint64_t arg) {
    pty_inst_t *p = pty_of(n);
    // truncate to 32 bits
    switch ((uint32_t) req) {
    case 0x80045430: /* TIOCGPTN */
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(int))) return -EFAULT;
        *(int *) (uintptr_t) arg = p ? p->num : 0;
        return 0;
    case 0x40045431: /* TIOCSPTLCK */
        return 0;
    case 0x80045439: /* TIOCGPTLCK */
    case 0x80045432: /* TIOCGDEV */
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(int))) return -EFAULT;
        *(int *) (uintptr_t) arg = 0;
        return 0;
    case 0x541B: /* FIONREAD */
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(int))) return -EFAULT;
        *(int *) (uintptr_t) arg = (int) ((p && p->s2m) ? p->s2m->count : 0);
        return 0;
    }
    return pty_common_ioctl(p, req, arg);
}

static void ptym_close(vfs_node_t *n) {
    pty_inst_t *p = pty_of(n);
    if (!p) return;
    if (p->s2m) {
        p->s2m->write_refs = 0;
        pipe_free(p->s2m);
        p->s2m = NULL;
    }
    if (p->m2s) {
        pipe_free(p->m2s);
        p->m2s = NULL;
    }
    if (p->slave) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/pts/%d", p->num);
        vfs_unlink(path);
        p->slave = NULL;
    }
    p->used = 0;
}

// slave rw
static int64_t ptys_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) off;
    pty_inst_t *p = pty_of(n);
    return p ? pipe_read(p->m2s, buf, len) : -(int64_t) EINVAL;
}

static int64_t ptys_write(vfs_node_t *n, const char *buf, uint64_t len, uint64_t pos) {
    pty_inst_t *p = pty_of(n);
    if (!p || !p->s2m) return -(int64_t) EINVAL;
    int64_t done = 0;
    for (uint64_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            static const char cr = '\r';
            pipe_write(p->s2m, &cr, 1);
        }
        pipe_write(p->s2m, buf + i, 1);
        done++;
    }
    return done;
}

static bool ptys_pollin(vfs_node_t *n) {
    pty_inst_t *p = pty_of(n);
    return p && p->m2s && p->m2s->count > 0;
}

// called when /dev/ptmx is opened
static int ptmx_open(vfs_node_t *n, int flags) {
    (void) n;
    (void) flags;
    int idx = -1;
    for (int i = 0; i < PTY_MAX; i++) {
        if (!g_ptys[i].used) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -ENOSPC;

    pty_inst_t *pty = &g_ptys[idx];
    pty->m2s = pipe_alloc();
    pty->s2m = pipe_alloc();
    if (!pty->m2s || !pty->s2m) {
        if (pty->m2s) pipe_free(pty->m2s);
        if (pty->s2m) pipe_free(pty->s2m);
        pty->m2s = pty->s2m = NULL;
        return -ENOMEM;
    }
    pty->m2s->read_refs = pty->m2s->write_refs = 1;
    pty->s2m->read_refs = pty->s2m->write_refs = 1;
    pty->num = idx;
    pty->used = 1;
    pty->pgid = g_current_proc ? (int) g_current_proc->pgid : 0;
    memset(&pty->termios, 0, sizeof(pty->termios));
    pty->termios.c_iflag = 0x2500;  /* ICRNL | IXON | IUTF8 */
    pty->termios.c_oflag = 0x5;     /* OPOST | ONLCR */
    pty->termios.c_cflag = 0xBF;    /* B38400 | CS8 | CREAD */
    pty->termios.c_lflag = 0x8A3B;  /* ISIG ICANON ECHO ECHOE ECHOK IEXTEN */
    pty->termios.c_cc[0] = 3;       /* VINTR  ^C */
    pty->termios.c_cc[1] = 28;      /* VQUIT  ^\ */
    pty->termios.c_cc[2] = 127;     /* VERASE DEL */
    pty->termios.c_cc[3] = 21;      /* VKILL  ^U */
    pty->termios.c_cc[4] = 4;       /* VEOF   ^D */
    pty->termios.c_cc[6] = 1;       /* VMIN */
    pty->ws.ws_row = 24;
    pty->ws.ws_col = 80;

    char slave_path[32];
    snprintf(slave_path, sizeof(slave_path), "/dev/pts/%d", idx);
    vfs_unlink(slave_path);
    pty->slave = vfs_create_chr(slave_path, ptys_read, ptys_write);
    if (pty->slave) {
        pty->slave->chr_pollin = ptys_pollin;
        pty->slave->chr_ioctl = ptys_ioctl;
        pty->slave->rdev = VFS_MKDEV(136, (uint32_t) idx); /* UNIX98_PTY_SLAVE_MAJOR */
        pty->slave->size = (uint64_t) idx;
        pty->slave->mode = S_IFCHR | 0620;
    }

    // init ephemeral master node (not in vfs tree)
    vfs_node_t *mn = &pty->master_node;
    memset(mn, 0, sizeof(*mn));
    mn->type = VFS_TYPE_CHR;
    mn->mode = S_IFCHR | 0620;
    mn->size = (uint64_t) idx;
    mn->chr_read = ptym_read;
    mn->chr_write = ptym_write;
    mn->chr_ioctl = ptym_ioctl;
    mn->chr_pollin = ptym_pollin;
    mn->chr_close = ptym_close;

    return fd_open_node(mn, O_RDWR);
}

void devfs_init(void) {
    vfs_mkdir_p("/dev", 0755);
    vfs_mkdir_p("/dev/input", 0755);

    struct {
        const char *path;
        int64_t (*rfn)(vfs_node_t *, char *, uint64_t, uint64_t);
        int64_t (*wfn)(vfs_node_t *, const char *, uint64_t, uint64_t);
        uint32_t rdev;
    } mem_devs[] = {
        { "/dev/null", dev_null_read, dev_null_write, VFS_MKDEV(1, 3) },
        { "/dev/zero", dev_zero_read, dev_null_write, VFS_MKDEV(1, 5) },
        { "/dev/urandom", dev_urandom_read, dev_random_write, VFS_MKDEV(1, 9) },
        { "/dev/random", dev_urandom_read, dev_random_write, VFS_MKDEV(1, 8) },
    };
    for (unsigned i = 0; i < sizeof(mem_devs) / sizeof(mem_devs[0]); i++) {
        vfs_node_t *n = vfs_create_chr(mem_devs[i].path, mem_devs[i].rfn, mem_devs[i].wfn);
        if (n) n->rdev = mem_devs[i].rdev;
    }
    {
        vfs_node_t *mn = vfs_create_chr("/dev/mem", dev_null_read, dev_null_write);
        if (mn) {
            mn->mode = S_IFCHR | 0600;
            mn->chr_mmap = devmem_mmap;
            mn->rdev = VFS_MKDEV(1, 1);
        }
    }

    vfs_mkdir_p("/dev/pts", 0755);
    {
        vfs_node_t *ptmx = vfs_create_chr("/dev/ptmx", NULL, NULL);
        if (ptmx) {
            ptmx->mode = S_IFCHR | 0666;
            ptmx->chr_open = ptmx_open;
            ptmx->rdev = VFS_MKDEV(5, 2);
        }
    }

    {
        vfs_node_t *tty = vfs_create_chr("/dev/tty", dev_tty_read, dev_tty_write);
        if (tty) tty->rdev = VFS_MKDEV(5, 0);
    }
    vfs_create_chr("/dev/stdin", dev_tty_read, dev_null_write);
    vfs_create_chr("/dev/stdout", dev_null_read, dev_tty_write);
    vfs_create_chr("/dev/stderr", dev_null_read, dev_tty_write);
    vfs_create_symlink("/dev/console", "/dev/tty");
    vfs_create_symlink("/dev/fd", "/proc/self/fd");
}
