#include "test_harness.h"

#include <sys/mount.h>

#ifndef LOOP_SET_FD
#define LOOP_SET_FD 0x4C00
#define LOOP_CLR_FD 0x4C01
#define LOOP_SET_STATUS64 0x4C04
#define LOOP_GET_STATUS64 0x4C05
#define LOOP_CONFIGURE 0x4C0A
#endif

#ifndef BLKGETSIZE64
#define BLKGETSIZE64 0x80081272
#endif

#define LO_NAME_SIZE 64
#define LO_KEY_SIZE 32

typedef struct {
    uint64_t lo_device, lo_inode, lo_rdevice, lo_offset, lo_sizelimit;
    uint32_t lo_number, lo_encrypt_type, lo_encrypt_key_size, lo_flags;
    uint8_t lo_file_name[LO_NAME_SIZE];
    uint8_t lo_crypt_name[LO_NAME_SIZE];
    uint8_t lo_encrypt_key[LO_KEY_SIZE];
    uint64_t lo_init[2];
} loop_info64_t;

typedef struct {
    int32_t fd;
    uint32_t block_size;
    loop_info64_t info;
    uint64_t reserved[8];
} loop_config_t;

#define AIMNT "/aimnt"
#define APPIMG "/app.img"
#define APPIMG_OFFSET appimg_off()

static off_t appimg_off(void) {
    off_t off = appimg_payload_offset(APPIMG);
    return off < 0 ? 0 : off;
}

static int mnt_dir_ok(const char *dir) {
    struct stat st;
    return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static int loop_mount_ok(void) {
    char buf[64];
    return mnt_dir_ok(AIMNT) && read_file(AIMNT "/hello.txt", buf, sizeof(buf)) ==
                                      (ssize_t) strlen("hello from squashfs\n");
}

static int loop_bind_file(const char *dev, const char *img, uint64_t offset) {
    int fd = open(img, O_RDONLY);
    if (fd < 0) return -1;
    int lfd = open(dev, O_RDONLY);
    if (lfd < 0) {
        close(fd);
        return -1;
    }
    loop_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.fd = fd;
    cfg.info.lo_offset = offset;
    int rc = ioctl(lfd, LOOP_CONFIGURE, &cfg);
    close(fd);
    close(lfd);
    return rc;
}

static int loop_clear(const char *dev) {
    int lfd = open(dev, O_RDONLY);
    if (lfd < 0) return -1;
    int rc = ioctl(lfd, LOOP_CLR_FD);
    close(lfd);
    return rc;
}

int test_loop_nodes_exist(void) {
    for (int i = 0; i < 4; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/loop%d", i);
        struct stat st;
        ASSERT_EQ(0, stat(path, &st));
        ASSERT_TRUE(S_ISCHR(st.st_mode));
    }
    return 1;
}
REGISTER_TEST(loop_nodes_exist, "Phase 6: Loop");

int test_loop_unbound_state(void) {
    int fd = open("/dev/loop1", O_RDONLY);
    ASSERT_GE(fd, 0);

    unsigned long sz = 12345;
    ASSERT_EQ(0, ioctl(fd, BLKGETSIZE64, &sz));
    ASSERT_EQ(0, (int) sz);

    char buf[512];
    ASSERT_EQ(0, (int) read(fd, buf, sizeof(buf)));

    ASSERT_EQ(-1, ioctl(fd, LOOP_CLR_FD));
    ASSERT_ERRNO(ENXIO);

    close(fd);
    return 1;
}
REGISTER_TEST(loop_unbound_state, "Phase 6: Loop");

int test_loop_configure_bad_args(void) {
    int lfd = open("/dev/loop2", O_RDONLY);
    ASSERT_GE(lfd, 0);

    int dirfd = open("/", O_RDONLY | O_DIRECTORY);
    ASSERT_GE(dirfd, 0);
    loop_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.fd = dirfd;
    ASSERT_EQ(-1, ioctl(lfd, LOOP_CONFIGURE, &cfg));
    ASSERT_ERRNO(EINVAL);
    close(dirfd);

    int img = open(APPIMG, O_RDONLY);
    ASSERT_GE(img, 0);
    memset(&cfg, 0, sizeof(cfg));
    cfg.fd = 999;
    ASSERT_EQ(-1, ioctl(lfd, LOOP_CONFIGURE, &cfg));
    ASSERT_ERRNO(EBADF);

    memset(&cfg, 0, sizeof(cfg));
    cfg.fd = img;
    cfg.info.lo_offset = 1 << 30;
    ASSERT_EQ(-1, ioctl(lfd, LOOP_CONFIGURE, &cfg));
    ASSERT_ERRNO(EINVAL);

    memset(&cfg, 0, sizeof(cfg));
    cfg.fd = img;
    cfg.block_size = 4096;
    ASSERT_EQ(-1, ioctl(lfd, LOOP_CONFIGURE, &cfg));
    ASSERT_ERRNO(EINVAL);

    ASSERT_EQ(-1, ioctl(lfd, LOOP_GET_STATUS64, NULL));
    close(img);
    close(lfd);
    return 1;
}
REGISTER_TEST(loop_configure_bad_args, "Phase 6: Loop");

int test_loop_configure_get_status(void) {
    int img = open(APPIMG, O_RDONLY);
    ASSERT_GE(img, 0);
    int lfd = open("/dev/loop3", O_RDONLY);
    ASSERT_GE(lfd, 0);

    loop_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.fd = img;
    cfg.info.lo_offset = APPIMG_OFFSET;
    ASSERT_EQ(0, ioctl(lfd, LOOP_CONFIGURE, &cfg));

    loop_info64_t li;
    memset(&li, 0, sizeof(li));
    ASSERT_EQ(0, ioctl(lfd, LOOP_GET_STATUS64, &li));
    ASSERT_EQ((uint64_t) APPIMG_OFFSET, li.lo_offset);
    ASSERT_EQ(3, (int) li.lo_number);

    unsigned long sz = 0;
    ASSERT_EQ(0, ioctl(lfd, BLKGETSIZE64, &sz));
    struct stat st;
    ASSERT_EQ(0, fstat(img, &st));
    unsigned long expect = (unsigned long) ((st.st_size - APPIMG_OFFSET + 511) / 512 * 512);
    ASSERT_EQ((int) expect, (int) sz);

    ASSERT_EQ(-1, ioctl(lfd, LOOP_SET_FD, img));
    ASSERT_ERRNO(EBUSY);

    ASSERT_EQ(0, ioctl(lfd, LOOP_CLR_FD));
    sz = 12345;
    ASSERT_EQ(0, ioctl(lfd, BLKGETSIZE64, &sz));
    ASSERT_EQ(0, (int) sz);

    close(img);
    close(lfd);
    return 1;
}
REGISTER_TEST(loop_configure_get_status, "Phase 6: Loop");

int test_loop_mount_appimage(void) {
    ASSERT_EQ(0, loop_bind_file("/dev/loop0", APPIMG, APPIMG_OFFSET));
    ASSERT_EQ(0, mount("/dev/loop0", AIMNT, "squashfs", MS_RDONLY, NULL));

    char buf[64];
    ASSERT_EQ((ssize_t) strlen("hello from squashfs\n"),
              read_file(AIMNT "/hello.txt", buf, sizeof(buf)));
    ASSERT_STREQ("hello from squashfs\n", buf);

    struct stat st;
    ASSERT_EQ(0, stat(AIMNT "/big.bin", &st));
    ASSERT_EQ(300000, (int) st.st_size);
    ASSERT_EQ(0, stat(AIMNT "/bin", &st));
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    ASSERT_EQ(5, (int) read_file(AIMNT "/usr/share/doc/readme.md", buf, sizeof(buf)));
    ASSERT_STREQ("docs\n", buf);

    char lbuf[PATH_MAX];
    ssize_t n = readlink(AIMNT "/link.txt", lbuf, sizeof(lbuf) - 1);
    ASSERT_GT(n, 0);
    lbuf[n] = '\0';
    ASSERT_STREQ("hello.txt", lbuf);

    ASSERT_EQ(0, umount2(AIMNT, 0));
    ASSERT_EQ(-1, stat(AIMNT "/hello.txt", &st));
    ASSERT_ERRNO(ENOENT);

    ASSERT_EQ(0, loop_clear("/dev/loop0"));
    return 1;
}
REGISTER_TEST(loop_mount_appimage, "Phase 6: Loop");

int test_loop_mount_appimage_again(void) {
    ASSERT_EQ(0, loop_bind_file("/dev/loop0", APPIMG, APPIMG_OFFSET));
    ASSERT_EQ(0, mount("/dev/loop0", AIMNT, "squashfs", MS_RDONLY, NULL));
    ASSERT_TRUE(loop_mount_ok());

    char buf[128];
    char *argv[] = { "/bin/sh", AIMNT "/AppRun", NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    ASSERT_NOTNULL(strstr(buf, "AppRun executed\n"));

    ASSERT_EQ(0, umount2(AIMNT, 0));
    ASSERT_EQ(0, loop_clear("/dev/loop0"));
    return 1;
}
REGISTER_TEST(loop_mount_appimage_again, "Phase 6: Loop");

int test_loop_mount_readonly(void) {
    ASSERT_EQ(0, loop_bind_file("/dev/loop0", APPIMG, APPIMG_OFFSET));
    ASSERT_EQ(0, mount("/dev/loop0", AIMNT, "squashfs", MS_RDONLY, NULL));
    ASSERT_TRUE(loop_mount_ok());

    int fd = open(AIMNT "/hello.txt", O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) {
        close(fd);
        ASSERT_TRUE(0);
    }
    else {
        ASSERT_ERRNO(EROFS);
    }

    ASSERT_EQ(-1, unlink(AIMNT "/hello.txt"));
    ASSERT_EQ(-1, rename(AIMNT "/hello.txt", AIMNT "/renamed.txt"));

    ASSERT_EQ(0, umount2(AIMNT, 0));
    ASSERT_EQ(0, loop_clear("/dev/loop0"));
    return 1;
}
REGISTER_TEST(loop_mount_readonly, "Phase 6: Loop");

int test_loop_mount_errors(void) {
    ASSERT_EQ(-1, mount("/dev/loop5", "/aimnt_err", "squashfs", MS_RDONLY, NULL));
    ASSERT_ERRNO(ENODEV);

    ASSERT_EQ(-1, mount("/dev/nosuch", "/aimnt_err", "squashfs", MS_RDONLY, NULL));
    ASSERT_ERRNO(ENODEV);

    ASSERT_EQ(-1, mount("/dev/loop1", "/aimnt_err", "ext2", 0, NULL));
    ASSERT_ERRNO(ENODEV);

    ASSERT_EQ(-1, mount("/dev/sdb", "/aimnt_err", "xfs", 0, NULL));
    ASSERT_ERRNO(ENODEV);

    ASSERT_EQ(0, loop_bind_file("/dev/loop4", APPIMG, 3 * 512));
    ASSERT_EQ(-1, mount("/dev/loop4", "/aimnt_err", "squashfs", MS_RDONLY, NULL));
    ASSERT_ERRNO(ENODEV);
    ASSERT_EQ(0, loop_clear("/dev/loop4"));
    return 1;
}
REGISTER_TEST(loop_mount_errors, "Phase 6: Loop");

int test_loop_mount_direct_block(void) {
    ASSERT_EQ(0, mount("/dev/sdb", "/aimnt_s", "squashfs", MS_RDONLY, NULL));

    char buf[64];
    ASSERT_EQ((ssize_t) strlen("hello from squashfs\n"),
              read_file("/aimnt_s/hello.txt", buf, sizeof(buf)));
    ASSERT_STREQ("hello from squashfs\n", buf);

    ASSERT_EQ(0, umount2("/aimnt_s", 0));
    struct stat st;
    ASSERT_EQ(-1, stat("/aimnt_s/hello.txt", &st));
    ASSERT_ERRNO(ENOENT);
    return 1;
}
REGISTER_TEST(loop_mount_direct_block, "Phase 6: Loop");

int test_loop_ext2_backing(void) {
    if (!mnt_dir_ok("/mnt")) return 1;

    int in = open(APPIMG, O_RDONLY);
    ASSERT_GE(in, 0);
    int out = open("/mnt/app2.img", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(out, 0);
    char copy[4096];
    ssize_t n;
    while ((n = read(in, copy, sizeof(copy))) > 0) {
        ASSERT_EQ((int) n, (int) write(out, copy, (size_t) n));
    }
    close(out);
    close(in);

    ASSERT_EQ(0, loop_bind_file("/dev/loop6", "/mnt/app2.img", APPIMG_OFFSET));
    ASSERT_EQ(0, mount("/dev/loop6", "/aimnt_e", "squashfs", MS_RDONLY, NULL));

    struct stat st;
    ASSERT_EQ(0, stat("/aimnt_e/big.bin", &st));
    ASSERT_EQ(300000, (int) st.st_size);

    char *buf = malloc(4096);
    ASSERT_NOTNULL(buf);
    int fd = open("/aimnt_e/big.bin", O_RDONLY);
    ASSERT_GE(fd, 0);
    ssize_t total = 0;
    while (total < 4096) {
        ssize_t r = read(fd, buf + total, 4096 - total);
        if (r <= 0) break;
        total += r;
    }
    close(fd);
    ASSERT_EQ(4096, (int) total);
    int ok = 1;
    for (int i = 0; i < 4096 && ok; i++) {
        if ((unsigned char) buf[i] != (unsigned char) ((i * 31 + 7) & 0xFF)) ok = 0;
    }
    free(buf);
    ASSERT_TRUE(ok);

    ASSERT_EQ(0, umount2("/aimnt_e", 0));
    ASSERT_EQ(0, loop_clear("/dev/loop6"));
    ASSERT_EQ(0, unlink("/mnt/app2.img"));
    return 1;
}
REGISTER_TEST(loop_ext2_backing, "Phase 6: Loop");
