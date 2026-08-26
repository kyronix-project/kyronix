#include "test_harness.h"

int test_getrandom(void) {
    char buf[16];

    /* basic */
    ssize_t n = syscall(SYS_getrandom, buf, sizeof(buf), 0);
    if (n < 0 && (errno == ENOSYS || errno == ENOTSUP)) return 1;
    ASSERT_EQ((ssize_t) sizeof(buf), n);

    /* GRND_NONBLOCK */
    n = syscall(SYS_getrandom, buf, sizeof(buf), 1);
    if (n < 0 && errno == ENOSYS) return 1;
    ASSERT_EQ((ssize_t) sizeof(buf), n);

    /* GRND_RANDOM */
    n = syscall(SYS_getrandom, buf, sizeof(buf), 2);
    if (n < 0 && (errno == ENOSYS || errno == EPERM)) return 1;
    ASSERT_EQ((ssize_t) sizeof(buf), n);

    /* large buffer (should be capped or succeed) */
    char big[4096];
    n = syscall(SYS_getrandom, big, sizeof(big), 0);
    if (n < 0 && errno == ENOSYS) return 1;
    ASSERT_GT(n, 0);

    return 1;
}
REGISTER_TEST(getrandom, "Phase 8: Random / Misc");

int test_dev_urandom(void) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 1; /* device optional */

    unsigned char a[64], b[64];
    ssize_t na = read(fd, a, sizeof(a));
    ssize_t nb = read(fd, b, sizeof(b));
    close(fd);
    ASSERT_EQ((ssize_t) sizeof(a), na);
    ASSERT_EQ((ssize_t) sizeof(b), nb);

    int all_zero = 1, all_same = 1;
    for (size_t i = 0; i < sizeof(a); i++) {
        if (a[i] != 0) all_zero = 0;
        if (a[i] != a[0]) all_same = 0;
    }
    ASSERT_FALSE(all_zero);             /* not a constant stream */
    ASSERT_FALSE(all_same);
    ASSERT_NE(0, memcmp(a, b, sizeof(a))); /* successive reads differ */
    return 1;
}
REGISTER_TEST(dev_urandom, "Phase 8: Random / Misc");

int test_set_tid_address(void) {
    int tid = 0;
    int *clear_child_tid = &tid;

    long ret = syscall(SYS_set_tid_address, clear_child_tid);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) return 1;
    ASSERT_GT(ret, 0); /* returns caller's TID */

    return 1;
}
REGISTER_TEST(set_tid_address, "Phase 8: Random / Misc");

int test_robust_list(void) {
    char head[24] __attribute__((aligned(8)));
    void *head_ptr;
    size_t len;

    memset(head, 0, sizeof(head));

    long ret = syscall(SYS_set_robust_list, head, sizeof(head));
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) return 1;
    ASSERT_EQ(0, ret);

    ret = syscall(SYS_get_robust_list, 0, &head_ptr, &len);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) return 1;
    ASSERT_EQ(0, ret);
    ASSERT_EQ(head_ptr, (void *) head);
    ASSERT_EQ(len, sizeof(head));

    return 1;
}
REGISTER_TEST(robust_list, "Phase 8: Random / Misc");

int test_close_range(void) {
    int fds[4];
    for (int i = 0; i < 4; i++) {
        fds[i] = dup(0);
        ASSERT_GE(fds[i], 0);
    }

    long ret = syscall(SYS_close_range, fds[0], fds[2], 0);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) {
        /* clean up manually */
        for (int i = 0; i < 4; i++) close(fds[i]);
        return 1;
    }
    ASSERT_EQ(0, ret);

    /* first 3 should be closed, last should still be open */
    ASSERT_EQ(-1, fcntl(fds[0], F_GETFD));
    ASSERT(errno == EBADF);
    ASSERT_EQ(-1, fcntl(fds[1], F_GETFD));
    ASSERT(errno == EBADF);
    ASSERT_EQ(-1, fcntl(fds[2], F_GETFD));
    ASSERT(errno == EBADF);
    ASSERT_GE(fcntl(fds[3], F_GETFD), 0);

    close(fds[3]);
    return 1;
}
REGISTER_TEST(close_range, "Phase 8: Random / Misc");

int test_prlimit(void) {
    struct rlimit old, new;

    /* getrlimit RLIMIT_NOFILE */
    ASSERT_EQ(0, getrlimit(RLIMIT_NOFILE, &old));
    ASSERT_GT(old.rlim_cur, 0);

    /* setrlimit to same value */
    new.rlim_cur = old.rlim_cur;
    new.rlim_max = old.rlim_max;
    ASSERT_EQ(0, setrlimit(RLIMIT_NOFILE, &new));

    /* prlimit64 on self */
    ASSERT_EQ(0, prlimit(0, RLIMIT_NOFILE, NULL, &old));
    ASSERT_GT(old.rlim_cur, 0);

    /* RLIMIT_NPROC */
    ASSERT_EQ(0, getrlimit(RLIMIT_NPROC, &old));
    ASSERT_GT(old.rlim_cur, 0);

    /* RLIMIT_DATA */
    int ret = getrlimit(RLIMIT_DATA, &old);
    if (ret < 0 && (errno == ENOSYS || errno == EINVAL)) return 1;
    ASSERT_EQ(0, ret);

    return 1;
}
REGISTER_TEST(prlimit, "Phase 8: Random / Misc");

/* linux/fb.h is not available to musl-gcc here, so mirror the ABI locally */
#define KFBIOGET_VSCREENINFO 0x4600
#define KFBIOGET_FSCREENINFO 0x4602

struct kfb_bitfield {
    uint32_t offset, length, msb_right;
};

struct kfb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct kfb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags, pixclock;
    uint32_t left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};

struct kfb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities, reserved[2];
};

/* A display server's first contact with the framebuffer: probe, then map it. */
int test_fbdev_screeninfo(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) return 1; /* no framebuffer on this boot */

    struct kfb_fix_screeninfo fix;
    struct kfb_var_screeninfo var;
    memset(&fix, 0xAA, sizeof(fix));
    memset(&var, 0xAA, sizeof(var));

    ASSERT_EQ(0, ioctl(fd, KFBIOGET_FSCREENINFO, &fix));
    ASSERT_EQ(0, ioctl(fd, KFBIOGET_VSCREENINFO, &var));

    ASSERT_GT(var.xres, 0u);
    ASSERT_GT(var.yres, 0u);
    ASSERT_TRUE(var.bits_per_pixel == 32 || var.bits_per_pixel == 24);
    ASSERT_EQ(0u, (unsigned) var.grayscale);
    ASSERT_EQ(0u, (unsigned) fix.type);   /* FB_TYPE_PACKED_PIXELS */
    ASSERT_EQ(2u, (unsigned) fix.visual); /* FB_VISUAL_TRUECOLOR */
    ASSERT_GT(fix.line_length, 0u);
    ASSERT_GE(fix.smem_len, fix.line_length * var.yres);
    ASSERT_EQ(0u, (unsigned) var.red.msb_right);
    ASSERT_GT(var.red.length, 0u);

    /* weston's fbdev backend maps the whole thing MAP_SHARED, write-only */
    void *fb = mmap(NULL, fix.smem_len, PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_TRUE(fb != MAP_FAILED);
    memset(fb, 0, 4096);
    ASSERT_EQ(0, munmap(fb, fix.smem_len));

    close(fd);
    return 1;
}
REGISTER_TEST(fbdev_screeninfo, "Phase 8: Random / Misc");
