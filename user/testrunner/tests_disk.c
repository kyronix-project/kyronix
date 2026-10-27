#include "test_harness.h"

#define MNT "/mnt"

static int mnt_ok(void) {
    struct stat st;
    return stat(MNT, &st) == 0 && S_ISDIR(st.st_mode);
}

int test_ahci_disk_mounted(void) {
    struct stat st;
    ASSERT_EQ(0, stat(MNT, &st));
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    return 1;
}
REGISTER_TEST(ahci_disk_mounted, "Phase 6: Disk");

int test_ahci_rw_512(void) {
    if (!mnt_ok()) return 1;
    const char *path = MNT "/ahci_rw512";
    char wbuf[512], rbuf[512];
    for (int i = 0; i < 512; i++) wbuf[i] = (char) (i & 0xFF);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(512, write(fd, wbuf, 512));
    close(fd);

    fd = open(path, O_RDONLY);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(512, read(fd, rbuf, 512));
    close(fd);
    unlink(path);

    ASSERT_EQ(0, memcmp(wbuf, rbuf, 512));
    return 1;
}
REGISTER_TEST(ahci_rw_512, "Phase 6: Disk");

int test_ahci_rw_large(void) {
    if (!mnt_ok()) return 1;
    const char *path = MNT "/ahci_rw_large";
    const int SIZE = 128 * 512 + 512;

    char *wbuf = malloc(SIZE);
    ASSERT_NOTNULL(wbuf);
    for (int i = 0; i < SIZE; i++) wbuf[i] = (char) (i * 7 + 13);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(wbuf);
        return 0;
    }
    ssize_t n = write(fd, wbuf, SIZE);
    close(fd);
    if (n != SIZE) {
        free(wbuf);
        unlink(path);
        return 0;
    }

    char *rbuf = malloc(SIZE);
    if (!rbuf) {
        free(wbuf);
        unlink(path);
        return 0;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(wbuf);
        free(rbuf);
        unlink(path);
        return 0;
    }
    n = read(fd, rbuf, SIZE);
    close(fd);

    int ok = (n == SIZE) && memcmp(wbuf, rbuf, SIZE) == 0;
    free(wbuf);
    free(rbuf);
    unlink(path);
    ASSERT_TRUE(ok);
    return 1;
}
REGISTER_TEST(ahci_rw_large, "Phase 6: Disk");

int test_ext2_create_stat(void) {
    if (!mnt_ok()) return 1;
    const char *path = MNT "/ext2_cstat";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    close(fd);

    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_TRUE(S_ISREG(st.st_mode));
    ASSERT_GT((long long) st.st_ino, 0LL);
    unlink(path);
    return 1;
}
REGISTER_TEST(ext2_create_stat, "Phase 6: Disk");

int test_ext2_write_read_text(void) {
    if (!mnt_ok()) return 1;
    const char *path = MNT "/ext2_text";
    const char *data = "hello from ext2\n";
    ASSERT_TRUE(write_file(path, data));

    char buf[64];
    ssize_t n = read_file(path, buf, sizeof(buf));
    ASSERT_EQ((ssize_t) strlen(data), n);
    ASSERT_STREQ(data, buf);
    unlink(path);
    return 1;
}
REGISTER_TEST(ext2_write_read_text, "Phase 6: Disk");

int test_ext2_write_binary(void) {
    if (!mnt_ok()) return 1;
    const char *path = MNT "/ext2_bin";
    const int SIZE = 4096;
    char *wbuf = malloc(SIZE);
    ASSERT_NOTNULL(wbuf);
    for (int i = 0; i < SIZE; i++) wbuf[i] = (char) ((i * 31 + 7) & 0xFF);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(SIZE, write(fd, wbuf, SIZE));
    close(fd);

    char *rbuf = malloc(SIZE);
    ASSERT_NOTNULL(rbuf);
    fd = open(path, O_RDONLY);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(SIZE, read(fd, rbuf, SIZE));
    close(fd);

    int ok = memcmp(wbuf, rbuf, SIZE) == 0;
    free(wbuf);
    free(rbuf);
    unlink(path);
    ASSERT_TRUE(ok);
    return 1;
}
REGISTER_TEST(ext2_write_binary, "Phase 6: Disk");

int test_ext2_overwrite_shrink(void) {
    if (!mnt_ok()) return 1;
    const char *path = MNT "/ext2_shrink";
    ASSERT_TRUE(write_file(path, "original longer content here"));
    ASSERT_TRUE(write_file(path, "short"));

    char buf[64];
    ssize_t n = read_file(path, buf, sizeof(buf));
    ASSERT_EQ(5, n);
    ASSERT_STREQ("short", buf);
    unlink(path);
    return 1;
}
REGISTER_TEST(ext2_overwrite_shrink, "Phase 6: Disk");

int test_ext2_append(void) {
    if (!mnt_ok()) return 1;
    const char *path = MNT "/ext2_append";

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(5, write(fd, "hello", 5));
    close(fd);

    fd = open(path, O_WRONLY | O_APPEND);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(6, write(fd, " world", 6));
    close(fd);

    char buf[32];
    ASSERT_EQ(11, read_file(path, buf, sizeof(buf)));
    ASSERT_STREQ("hello world", buf);
    unlink(path);
    return 1;
}
REGISTER_TEST(ext2_append, "Phase 6: Disk");

int test_ext2_stat_size(void) {
    if (!mnt_ok()) return 1;
    const char *path = MNT "/ext2_stsize";
    ASSERT_TRUE(write_file(path, "0123456789"));

    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_TRUE(S_ISREG(st.st_mode));
    ASSERT_EQ(10, (int) st.st_size);
    unlink(path);
    return 1;
}
REGISTER_TEST(ext2_stat_size, "Phase 6: Disk");

int test_ext2_mkdir(void) {
    if (!mnt_ok()) return 1;
    const char *dir = MNT "/ext2_mkdir";
    ASSERT_EQ(0, mkdir(dir, 0755));

    struct stat st;
    ASSERT_EQ(0, stat(dir, &st));
    ASSERT_TRUE(S_ISDIR(st.st_mode));

    ASSERT_EQ(0, rmdir(dir));
    ASSERT_EQ(-1, stat(dir, &st));
    ASSERT_ERRNO(ENOENT);
    return 1;
}
REGISTER_TEST(ext2_mkdir, "Phase 6: Disk");

int test_ext2_nested_dirs(void) {
    if (!mnt_ok()) return 1;
    const char *d1 = MNT "/ext2_nd";
    const char *d2 = MNT "/ext2_nd/sub";
    const char *d3 = MNT "/ext2_nd/sub/deep";

    ASSERT_EQ(0, mkdir(d1, 0755));
    ASSERT_EQ(0, mkdir(d2, 0755));
    ASSERT_EQ(0, mkdir(d3, 0755));

    struct stat st;
    ASSERT_EQ(0, stat(d3, &st));
    ASSERT_TRUE(S_ISDIR(st.st_mode));

    rmdir(d3);
    rmdir(d2);
    rmdir(d1);
    return 1;
}
REGISTER_TEST(ext2_nested_dirs, "Phase 6: Disk");

int test_ext2_file_in_subdir(void) {
    if (!mnt_ok()) return 1;
    const char *dir = MNT "/ext2_subfd";
    const char *file = MNT "/ext2_subfd/data";

    ASSERT_EQ(0, mkdir(dir, 0755));
    ASSERT_TRUE(write_file(file, "in subdir"));

    char buf[32];
    ASSERT_EQ(9, read_file(file, buf, sizeof(buf)));
    ASSERT_STREQ("in subdir", buf);

    unlink(file);
    rmdir(dir);
    return 1;
}
REGISTER_TEST(ext2_file_in_subdir, "Phase 6: Disk");

int test_ext2_symlink_fast(void) {
    if (!mnt_ok()) return 1;
    const char *target = MNT "/ext2_sym_tgt";
    const char *lnk = MNT "/ext2_sym_fast";

    ASSERT_TRUE(write_file(target, "target content"));
    ASSERT_EQ(0, symlink(target, lnk));

    char buf[PATH_MAX];
    ssize_t n = readlink(lnk, buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    ASSERT_STREQ(target, buf);

    char data[32];
    ASSERT_EQ(14, read_file(lnk, data, sizeof(data)));
    ASSERT_STREQ("target content", data);

    unlink(lnk);
    unlink(target);
    return 1;
}
REGISTER_TEST(ext2_symlink_fast, "Phase 6: Disk");

int test_ext2_symlink_slow(void) {
    if (!mnt_ok()) return 1;
    const char *target = MNT
        "/this_is_a_very_long_symlink_target_path_that_will_not_fit_in_fast_symlink_iblock_area";
    const char *lnk = MNT "/ext2_sym_slow";

    ASSERT_EQ(0, symlink(target, lnk));

    char buf[PATH_MAX];
    ssize_t n = readlink(lnk, buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    ASSERT_STREQ(target, buf);

    unlink(lnk);
    return 1;
}
REGISTER_TEST(ext2_symlink_slow, "Phase 6: Disk");

int test_ext2_unlink(void) {
    if (!mnt_ok()) return 1;
    const char *path = MNT "/ext2_unlink";
    ASSERT_TRUE(write_file(path, "to delete"));

    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(0, unlink(path));
    ASSERT_EQ(-1, stat(path, &st));
    ASSERT_ERRNO(ENOENT);
    return 1;
}
REGISTER_TEST(ext2_unlink, "Phase 6: Disk");

int test_ext2_readdir(void) {
    if (!mnt_ok()) return 1;
    const char *dir = MNT "/ext2_rddir";
    ASSERT_EQ(0, mkdir(dir, 0755));

    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/alpha", dir);
    ASSERT_TRUE(write_file(p, "a"));
    snprintf(p, sizeof(p), "%s/beta", dir);
    ASSERT_TRUE(write_file(p, "b"));
    snprintf(p, sizeof(p), "%s/gamma", dir);
    ASSERT_TRUE(write_file(p, "c"));

    DIR *d = opendir(dir);
    ASSERT_NOTNULL(d);
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, "alpha") == 0 || strcmp(ent->d_name, "beta") == 0 ||
            strcmp(ent->d_name, "gamma") == 0)
            found++;
    }
    closedir(d);
    ASSERT_EQ(3, found);

    snprintf(p, sizeof(p), "%s/alpha", dir);
    unlink(p);
    snprintf(p, sizeof(p), "%s/beta", dir);
    unlink(p);
    snprintf(p, sizeof(p), "%s/gamma", dir);
    unlink(p);
    rmdir(dir);
    return 1;
}
REGISTER_TEST(ext2_readdir, "Phase 6: Disk");

int test_ext2_large_file(void) {
    if (!mnt_ok()) return 1;
    const char *path = MNT "/ext2_large";
    const int SIZE = 4096 * 13;

    char *wbuf = malloc(SIZE);
    ASSERT_NOTNULL(wbuf);
    for (int i = 0; i < SIZE; i++) wbuf[i] = (char) (i % 251);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(SIZE, write(fd, wbuf, SIZE));
    close(fd);

    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(SIZE, (int) st.st_size);

    char *rbuf = malloc(SIZE);
    ASSERT_NOTNULL(rbuf);
    fd = open(path, O_RDONLY);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(SIZE, read(fd, rbuf, SIZE));
    close(fd);

    int ok = memcmp(wbuf, rbuf, SIZE) == 0;
    free(wbuf);
    free(rbuf);
    unlink(path);
    ASSERT_TRUE(ok);
    return 1;
}
REGISTER_TEST(ext2_large_file, "Phase 6: Disk");

int test_ext2_many_files(void) {
    if (!mnt_ok()) return 1;
    const char *dir = MNT "/ext2_many";
    ASSERT_EQ(0, mkdir(dir, 0755));

    char path[PATH_MAX];
    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof(path), "%s/f%02d", dir, i);
        ASSERT_TRUE(write_file(path, "x"));
    }

    DIR *d = opendir(dir);
    ASSERT_NOTNULL(d);
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == 'f') count++;
    }
    closedir(d);
    ASSERT_EQ(32, count);

    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof(path), "%s/f%02d", dir, i);
        unlink(path);
    }
    rmdir(dir);
    return 1;
}
REGISTER_TEST(ext2_many_files, "Phase 6: Disk");
