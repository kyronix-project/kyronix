#include "test_harness.h"

#include <sys/mount.h>

#define SQFS "/sqfs"

static int sqfs_ok(void) {
    struct stat st;
    return stat(SQFS, &st) == 0 && S_ISDIR(st.st_mode);
}

int test_squashfs_mounted(void) {
    struct stat st;
    ASSERT_EQ(0, stat(SQFS, &st));
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    return 1;
}
REGISTER_TEST(squashfs_mounted, "Phase 6: SquashFS");

int test_squashfs_read_text(void) {
    if (!sqfs_ok()) return 1;
    char buf[64];
    ssize_t n = read_file(SQFS "/hello.txt", buf, sizeof(buf));
    ASSERT_EQ((ssize_t) strlen("hello from squashfs\n"), n);
    ASSERT_STREQ("hello from squashfs\n", buf);
    return 1;
}
REGISTER_TEST(squashfs_read_text, "Phase 6: SquashFS");

int test_squashfs_stat(void) {
    if (!sqfs_ok()) return 1;
    struct stat st;
    ASSERT_EQ(0, stat(SQFS "/hello.txt", &st));
    ASSERT_TRUE(S_ISREG(st.st_mode));
    ASSERT_EQ((int) strlen("hello from squashfs\n"), (int) st.st_size);
    ASSERT_EQ(0, stat(SQFS "/AppRun", &st));
    ASSERT_TRUE(S_ISREG(st.st_mode));
    ASSERT_TRUE(st.st_mode & 0111);
    ASSERT_EQ(0, stat(SQFS "/bin", &st));
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    return 1;
}
REGISTER_TEST(squashfs_stat, "Phase 6: SquashFS");

int test_squashfs_read_big(void) {
    if (!sqfs_ok()) return 1;
    const int SIZE = 300000;
    char *expect = malloc(SIZE);
    char *got = malloc(SIZE);
    ASSERT_NOTNULL(expect);
    ASSERT_NOTNULL(got);
    for (int i = 0; i < SIZE; i++) expect[i] = (char) ((i * 31 + 7) & 0xFF);

    int fd = open(SQFS "/big.bin", O_RDONLY);
    ASSERT_GE(fd, 0);
    ssize_t total = 0;
    while (total < SIZE) {
        ssize_t n = read(fd, got + total, SIZE - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    ASSERT_EQ(SIZE, (int) total);
    ASSERT_EQ(0, memcmp(expect, got, SIZE));
    free(expect);
    free(got);
    return 1;
}
REGISTER_TEST(squashfs_read_big, "Phase 6: SquashFS");

int test_squashfs_read_partial(void) {
    if (!sqfs_ok()) return 1;
    char buf[100];
    int fd = open(SQFS "/big.bin", O_RDONLY);
    ASSERT_GE(fd, 0);
    ASSERT_EQ((off_t) 131079, lseek(fd, 131079, SEEK_SET));
    ASSERT_EQ(100, (int) read(fd, buf, 100));
    close(fd);
    for (int i = 0; i < 100; i++) {
        int idx = 131079 + i;
        ASSERT_EQ((int) ((idx * 31 + 7) & 0xFF), (int) (unsigned char) buf[i]);
    }
    return 1;
}
REGISTER_TEST(squashfs_read_partial, "Phase 6: SquashFS");

int test_squashfs_read_tail(void) {
    if (!sqfs_ok()) return 1;
    char buf[128];
    int fd = open(SQFS "/big.bin", O_RDONLY);
    ASSERT_GE(fd, 0);
    ASSERT_EQ((off_t) 299872, lseek(fd, 299872, SEEK_SET));
    ASSERT_EQ(128, (int) read(fd, buf, 128));
    close(fd);
    for (int i = 0; i < 128; i++) {
        int idx = 299872 + i;
        ASSERT_EQ((int) ((idx * 31 + 7) & 0xFF), (int) (unsigned char) buf[i]);
    }
    return 1;
}
REGISTER_TEST(squashfs_read_tail, "Phase 6: SquashFS");

int test_squashfs_sparse(void) {
    if (!sqfs_ok()) return 1;
    const int SIZE = 1024 * 1024;
    struct stat st;
    ASSERT_EQ(0, stat(SQFS "/sparse.bin", &st));
    ASSERT_EQ(SIZE, (int) st.st_size);

    char *buf = malloc(SIZE);
    ASSERT_NOTNULL(buf);
    int fd = open(SQFS "/sparse.bin", O_RDONLY);
    ASSERT_GE(fd, 0);
    ssize_t total = 0;
    while (total < SIZE) {
        ssize_t n = read(fd, buf + total, SIZE - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    ASSERT_EQ(SIZE, (int) total);
    int ok = 1;
    for (int i = 0; i < 1024 && ok; i++) {
        if ((unsigned char) buf[i] != (unsigned char) ((i * 13 + 1) & 0xFF)) ok = 0;
    }
    for (int i = 1024; i < SIZE && ok; i++) {
        if (buf[i] != 0) ok = 0;
    }
    free(buf);
    ASSERT_TRUE(ok);
    return 1;
}
REGISTER_TEST(squashfs_sparse, "Phase 6: SquashFS");

int test_squashfs_symlink(void) {
    if (!sqfs_ok()) return 1;
    char buf[PATH_MAX];
    ssize_t n = readlink(SQFS "/link.txt", buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    ASSERT_STREQ("hello.txt", buf);

    n = readlink(SQFS "/bin/up.txt", buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    ASSERT_STREQ("../hello.txt", buf);

    char data[64];
    ASSERT_EQ((ssize_t) strlen("hello from squashfs\n"), read_file(SQFS "/link.txt", data, sizeof(data)));
    ASSERT_STREQ("hello from squashfs\n", data);
    ASSERT_EQ((ssize_t) strlen("hello from squashfs\n"), read_file(SQFS "/bin/up.txt", data, sizeof(data)));
    ASSERT_STREQ("hello from squashfs\n", data);
    return 1;
}
REGISTER_TEST(squashfs_symlink, "Phase 6: SquashFS");

int test_squashfs_readdir(void) {
    if (!sqfs_ok()) return 1;
    DIR *d = opendir(SQFS);
    ASSERT_NOTNULL(d);
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, "hello.txt") == 0 || strcmp(ent->d_name, "AppRun") == 0 ||
            strcmp(ent->d_name, "big.bin") == 0 || strcmp(ent->d_name, "sparse.bin") == 0 ||
            strcmp(ent->d_name, "link.txt") == 0 || strcmp(ent->d_name, "bin") == 0 ||
            strcmp(ent->d_name, "usr") == 0)
            found++;
    }
    closedir(d);
    ASSERT_EQ(7, found);

    d = opendir(SQFS "/bin");
    ASSERT_NOTNULL(d);
    int bin_found = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, "tool.dat") == 0 || strcmp(ent->d_name, "up.txt") == 0)
            bin_found++;
    }
    closedir(d);
    ASSERT_EQ(2, bin_found);
    return 1;
}
REGISTER_TEST(squashfs_readdir, "Phase 6: SquashFS");

int test_squashfs_subdir(void) {
    if (!sqfs_ok()) return 1;
    char buf[32];
    ssize_t n = read_file(SQFS "/usr/share/doc/readme.md", buf, sizeof(buf));
    ASSERT_EQ(5, n);
    ASSERT_STREQ("docs\n", buf);
    return 1;
}
REGISTER_TEST(squashfs_subdir, "Phase 6: SquashFS");

int test_squashfs_subdir_data(void) {
    if (!sqfs_ok()) return 1;
    const int SIZE = 3000;
    char *expect = malloc(SIZE);
    char *got = malloc(SIZE);
    ASSERT_NOTNULL(expect);
    ASSERT_NOTNULL(got);
    for (int i = 0; i < SIZE; i++) expect[i] = (char) ((i * 7 + 3) & 0xFF);
    int fd = open(SQFS "/bin/tool.dat", O_RDONLY);
    ASSERT_GE(fd, 0);
    ssize_t total = 0;
    while (total < SIZE) {
        ssize_t n = read(fd, got + total, SIZE - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    ASSERT_EQ(SIZE, (int) total);
    ASSERT_EQ(0, memcmp(expect, got, SIZE));
    free(expect);
    free(got);
    return 1;
}
REGISTER_TEST(squashfs_subdir_data, "Phase 6: SquashFS");

int test_squashfs_exec_script(void) {
    if (!sqfs_ok()) return 1;
    char buf[128];
    char *argv[] = { "/bin/sh", SQFS "/AppRun", NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "AppRun executed\n"));
    return 1;
}
REGISTER_TEST(squashfs_exec_script, "Phase 6: SquashFS");

int test_squashfs_readonly(void) {
    if (!sqfs_ok()) return 1;
    int fd = open(SQFS "/hello.txt", O_WRONLY);
    if (fd >= 0) {
        ssize_t n = write(fd, "x", 1);
        ASSERT_EQ(-1, (int) n);
        ASSERT_ERRNO(EROFS);
        close(fd);
    } else {
        ASSERT_ERRNO(EROFS);
    }
    return 1;
}
REGISTER_TEST(squashfs_readonly, "Phase 6: SquashFS");

#ifndef LOOP_SET_FD
#define LOOP_CONFIGURE 0x4C0A
#define LOOP_CLR_FD 0x4C01
#endif

typedef struct {
    uint64_t lo_device, lo_inode, lo_rdevice, lo_offset, lo_sizelimit;
    uint32_t lo_number, lo_encrypt_type, lo_encrypt_key_size, lo_flags;
    uint8_t lo_file_name[64];
    uint8_t lo_crypt_name[64];
    uint8_t lo_encrypt_key[32];
    uint64_t lo_init[2];
} xz_loop_info64_t;

typedef struct {
    int32_t fd;
    uint32_t block_size;
    xz_loop_info64_t info;
    uint64_t reserved[8];
} xz_loop_config_t;

#define XZ_IMG "/app_xz.img"
#define XZ_OFF 4096
#define XZ_MNT "/sqfs_xz"

static int xz_img_ok(void) {
    struct stat st;
    return stat(XZ_IMG, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 4096;
}

static int xz_loop_bind(void) {
    int fd = open(XZ_IMG, O_RDONLY);
    if (fd < 0) return -1;
    int lfd = open("/dev/loop7", O_RDONLY);
    if (lfd < 0) {
        close(fd);
        return -1;
    }
    xz_loop_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.fd = fd;
    cfg.info.lo_offset = XZ_OFF;
    int rc = ioctl(lfd, LOOP_CONFIGURE, &cfg);
    close(fd);
    close(lfd);
    return rc;
}

static void xz_loop_teardown(void) {
    umount2(XZ_MNT, 0);
    int lfd = open("/dev/loop7", O_RDONLY);
    if (lfd >= 0) {
        ioctl(lfd, LOOP_CLR_FD);
        close(lfd);
    }
}

int test_squashfs_xz_mount(void) {
    if (!xz_img_ok()) return 1;
    ASSERT_EQ(0, mkdir(XZ_MNT, 0755));
    ASSERT_EQ(0, xz_loop_bind());
    ASSERT_EQ(0, mount("/dev/loop7", XZ_MNT, "squashfs", MS_RDONLY, NULL));

    char buf[64];
    ASSERT_EQ((ssize_t) strlen("hello from squashfs\n"),
              read_file(XZ_MNT "/hello.txt", buf, sizeof(buf)));
    ASSERT_STREQ("hello from squashfs\n", buf);

    struct stat st;
    ASSERT_EQ(0, stat(XZ_MNT "/big.bin", &st));
    ASSERT_EQ(300000, (int) st.st_size);
    ASSERT_EQ(0, stat(XZ_MNT "/bin", &st));
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    ASSERT_EQ(5, (int) read_file(XZ_MNT "/usr/share/doc/readme.md", buf, sizeof(buf)));
    ASSERT_STREQ("docs\n", buf);

    xz_loop_teardown();
    rmdir(XZ_MNT);
    return 1;
}
REGISTER_TEST(squashfs_xz_mount, "Phase 6: SquashFS");

int test_squashfs_xz_read_big(void) {
    if (!xz_img_ok()) return 1;
    ASSERT_EQ(0, mkdir(XZ_MNT, 0755));
    ASSERT_EQ(0, xz_loop_bind());
    ASSERT_EQ(0, mount("/dev/loop7", XZ_MNT, "squashfs", MS_RDONLY, NULL));

    const int SIZE = 300000;
    char *expect = malloc(SIZE);
    char *got = malloc(SIZE);
    ASSERT_NOTNULL(expect);
    ASSERT_NOTNULL(got);
    for (int i = 0; i < SIZE; i++) expect[i] = (char) ((i * 31 + 7) & 0xFF);

    int fd = open(XZ_MNT "/big.bin", O_RDONLY);
    ASSERT_GE(fd, 0);
    ssize_t total = 0;
    while (total < SIZE) {
        ssize_t n = read(fd, got + total, SIZE - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    ASSERT_EQ(SIZE, (int) total);
    ASSERT_EQ(0, memcmp(expect, got, SIZE));
    free(expect);
    free(got);

    xz_loop_teardown();
    rmdir(XZ_MNT);
    return 1;
}
REGISTER_TEST(squashfs_xz_read_big, "Phase 6: SquashFS");

int test_squashfs_xz_sparse(void) {
    if (!xz_img_ok()) return 1;
    ASSERT_EQ(0, mkdir(XZ_MNT, 0755));
    ASSERT_EQ(0, xz_loop_bind());
    ASSERT_EQ(0, mount("/dev/loop7", XZ_MNT, "squashfs", MS_RDONLY, NULL));

    const int SIZE = 1024 * 1024;
    struct stat st;
    ASSERT_EQ(0, stat(XZ_MNT "/sparse.bin", &st));
    ASSERT_EQ(SIZE, (int) st.st_size);

    char *buf = malloc(SIZE);
    ASSERT_NOTNULL(buf);
    int fd = open(XZ_MNT "/sparse.bin", O_RDONLY);
    ASSERT_GE(fd, 0);
    ssize_t total = 0;
    while (total < SIZE) {
        ssize_t n = read(fd, buf + total, SIZE - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    ASSERT_EQ(SIZE, (int) total);
    int ok = 1;
    for (int i = 0; i < 1024 && ok; i++) {
        if ((unsigned char) buf[i] != (unsigned char) ((i * 13 + 1) & 0xFF)) ok = 0;
    }
    for (int i = 1024; i < SIZE && ok; i++) {
        if (buf[i] != 0) ok = 0;
    }
    free(buf);
    ASSERT_TRUE(ok);

    xz_loop_teardown();
    rmdir(XZ_MNT);
    return 1;
}
REGISTER_TEST(squashfs_xz_sparse, "Phase 6: SquashFS");

int test_squashfs_xz_symlink_readdir(void) {
    if (!xz_img_ok()) return 1;
    ASSERT_EQ(0, mkdir(XZ_MNT, 0755));
    ASSERT_EQ(0, xz_loop_bind());
    ASSERT_EQ(0, mount("/dev/loop7", XZ_MNT, "squashfs", MS_RDONLY, NULL));

    // top-level symlink target string survives xz metadata decode
    char lbuf[PATH_MAX];
    ssize_t n = readlink(XZ_MNT "/link.txt", lbuf, sizeof(lbuf) - 1);
    ASSERT_GT(n, 0);
    lbuf[n] = '\0';
    ASSERT_STREQ("hello.txt", lbuf);

    // root directory listing fully populated from xz-compressed dir blocks
    DIR *d = opendir(XZ_MNT);
    ASSERT_NOTNULL(d);
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, "hello.txt") == 0 || strcmp(ent->d_name, "AppRun") == 0 ||
            strcmp(ent->d_name, "big.bin") == 0 || strcmp(ent->d_name, "sparse.bin") == 0 ||
            strcmp(ent->d_name, "link.txt") == 0 || strcmp(ent->d_name, "bin") == 0 ||
            strcmp(ent->d_name, "usr") == 0)
            found++;
    }
    closedir(d);
    ASSERT_EQ(7, found);

    // nested subdir listing + regular-file read through xz metadata + data blocks
    DIR *bd = opendir(XZ_MNT "/bin");
    ASSERT_NOTNULL(bd);
    int bin_found = 0;
    while ((ent = readdir(bd)) != NULL) {
        if (strcmp(ent->d_name, "tool.dat") == 0 || strcmp(ent->d_name, "up.txt") == 0)
            bin_found++;
    }
    closedir(bd);
    ASSERT_EQ(2, bin_found);

    const int SIZE = 3000;
    char *expect = malloc(SIZE);
    char *got = malloc(SIZE);
    ASSERT_NOTNULL(expect);
    ASSERT_NOTNULL(got);
    for (int i = 0; i < SIZE; i++) expect[i] = (char) ((i * 7 + 3) & 0xFF);
    int fd = open(XZ_MNT "/bin/tool.dat", O_RDONLY);
    ASSERT_GE(fd, 0);
    ssize_t total = 0;
    while (total < SIZE) {
        ssize_t r = read(fd, got + total, SIZE - total);
        if (r <= 0) break;
        total += r;
    }
    close(fd);
    ASSERT_EQ(SIZE, (int) total);
    ASSERT_EQ(0, memcmp(expect, got, SIZE));
    free(expect);
    free(got);

    // deep path (usr/share/doc) exercises multi-block metadata navigation
    char buf[32];
    ASSERT_EQ(5, (int) read_file(XZ_MNT "/usr/share/doc/readme.md", buf, sizeof(buf)));
    ASSERT_STREQ("docs\n", buf);

    xz_loop_teardown();
    rmdir(XZ_MNT);
    return 1;
}
REGISTER_TEST(squashfs_xz_symlink_readdir, "Phase 6: SquashFS");
