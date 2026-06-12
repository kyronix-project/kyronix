#include "fbdev.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../proc/proc.h"
#include "../syscall/syscall.h"
#include "fb.h"

typedef struct
{
    uint32_t offset, length, msb_right;
} fb_bitfield_t;

typedef struct
{
    uint32_t xres, yres;
    uint32_t xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    fb_bitfield_t red, green, blue, transp;
    uint32_t nonstd, activate;
    uint32_t height, width;
    uint32_t accel_flags, pixclock;
    uint32_t left_margin, right_margin;
    uint32_t upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len;
    uint32_t sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
} __attribute__((packed)) fb_var_t;

typedef struct
{
    char id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type; /* 0 = PACKED_PIXELS */
    uint32_t type_aux;
    uint32_t visual; /* 2 = TRUECOLOR */
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
} __attribute__((packed)) fb_fix_t;

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOBLANK 0x4611
#define EPERM 1

static int64_t fb0_ioctl(vfs_node_t* n, uint64_t req, uint64_t arg)
{
    (void) n;
    switch (req)
    {
    case FBIOGET_VSCREENINFO:
    {
        fb_var_t* v = (fb_var_t*) (uintptr_t) arg;
        if (!v)
            return -22;
        if (!uptr_ok_w(v, sizeof(*v)))
            return -14;
        memset(v, 0, sizeof(*v));
        v->xres = v->xres_virtual = (uint32_t) g_fb.width;
        v->yres = v->yres_virtual = (uint32_t) g_fb.height;
        v->bits_per_pixel = g_fb.bpp;
        if (g_fb.bpp == 32)
        {
            v->red = (fb_bitfield_t) {16, 8, 0};
            v->green = (fb_bitfield_t) {8, 8, 0};
            v->blue = (fb_bitfield_t) {0, 8, 0};
            v->transp = (fb_bitfield_t) {24, 8, 0};
        }
        else if (g_fb.bpp == 24)
        {
            v->red = (fb_bitfield_t) {16, 8, 0};
            v->green = (fb_bitfield_t) {8, 8, 0};
            v->blue = (fb_bitfield_t) {0, 8, 0};
        }
        v->activate = 0;
        v->height = v->width = 0xFFFFFFFF; /* unknown physical size */
        return 0;
    }
    case FBIOPUT_VSCREENINFO:
        return 0; /* accept but ignore mode changes */
    case FBIOGET_FSCREENINFO:
    {
        fb_fix_t* f = (fb_fix_t*) (uintptr_t) arg;
        if (!f)
            return -22;
        if (!uptr_ok_w(f, sizeof(*f)))
            return -14;
        memset(f, 0, sizeof(*f));
        memcpy(f->id, "kyronixfb", 9);
        f->smem_start = g_fb.phys_addr;
        f->smem_len = (uint32_t) (g_fb.pitch * g_fb.height);
        f->type = 0;   /* PACKED_PIXELS */
        f->visual = 2; /* TRUECOLOR */
        f->line_length = (uint32_t) g_fb.pitch;
        return 0;
    }
    case FBIOBLANK:
        return 0;
    default:
        return -22; /* EINVAL */
    }
}

static int64_t fb0_mmap(vfs_node_t* n, uint64_t off, uint64_t len, uint64_t va, uint64_t vflags)
{
    (void) n;
    (void) off;
    proc_t* p = g_current_proc;
    if (!p || !p->space)
        return -22;
    if (p->euid != 0)
        return -(int64_t) EPERM;
    fb_cursor_enable(0);
    uint64_t phys = g_fb.phys_addr;
    uint64_t flags = vflags | VMM_PRESENT | VMM_USER | VMM_WRITE;
    for (uint64_t o = 0; o < len; o += 0x1000)
        vmm_map(p->space, va + o, phys + o, flags);
    return (int64_t) va;
}

static int64_t fb0_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    uint64_t fb_size = g_fb.pitch * g_fb.height;
    if (off >= fb_size)
        return 0;
    uint64_t r = fb_size - off < len ? fb_size - off : len;
    __builtin_memcpy(buf, (uint8_t*) g_fb.addr + off, r);
    return (int64_t) r;
}

void fbdev_init(void)
{
    vfs_node_t* n = vfs_create_chr("/dev/fb0", fb0_read, NULL);
    if (!n)
        return;
    n->mode = S_IFCHR | 0600;
    n->chr_ioctl = fb0_ioctl;
    n->chr_mmap = fb0_mmap;
    vfs_create_symlink("/dev/fb", "/dev/fb0");
}
