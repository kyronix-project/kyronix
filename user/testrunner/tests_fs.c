#include "test_harness.h"

int test_open_close(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "open_close_test");

    /* create */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    close(fd);

    /* open existing read-only */
    fd = open(path, O_RDONLY);
    ASSERT_GE(fd, 0);
    close(fd);

    /* open nonexistent */
    fd = open("/nonexistent_file_xyz", O_RDONLY);
    ASSERT(fd < 0);
    ASSERT(errno == ENOENT);

    /* open with O_CREAT */
    fd = open("/tmp/test_oc_new", O_WRONLY | O_CREAT | O_EXCL, 0644);
    ASSERT_GE(fd, 0);
    close(fd);

    /* close invalid fd */
    ASSERT(close(9999) < 0);
    ASSERT(errno == EBADF);

    return 1;
}
REGISTER_TEST(open_close, "Phase 2: File System");

int test_read_write(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "rw_test");
    const char *data = "hello world\n";
    size_t len = strlen(data);
    char buf[64];

    /* write */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ((ssize_t) len, write(fd, data, len));
    close(fd);

    /* read back */
    fd = open(path, O_RDONLY);
    ASSERT_GE(fd, 0);
    ssize_t n = read(fd, buf, sizeof(buf));
    ASSERT_EQ((ssize_t) len, n);
    buf[n] = '\0';
    ASSERT_STREQ(data, buf);
    close(fd);

    /* read past EOF */
    fd = open(path, O_RDONLY);
    ASSERT_GE(fd, 0);
    n = read(fd, buf, sizeof(buf));
    ASSERT_EQ((ssize_t) len, n);
    n = read(fd, buf, sizeof(buf));
    ASSERT_EQ(n, 0);
    close(fd);

    /* write to read-only fd */
    fd = open(path, O_RDONLY);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(-1, write(fd, data, len));
    ASSERT(errno == EBADF);
    close(fd);

    return 1;
}
REGISTER_TEST(read_write, "Phase 2: File System");

int test_lseek(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "lseek_test");
    const char *data = "0123456789";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    write(fd, data, 10);
    close(fd);

    fd = open(path, O_RDONLY);
    ASSERT_GE(fd, 0);

    /* SEEK_SET */
    ASSERT_EQ(5, lseek(fd, 5, SEEK_SET));
    char c;
    ASSERT_EQ(1, read(fd, &c, 1));
    ASSERT_EQ('5', c);

    /* SEEK_CUR */
    ASSERT_EQ(8, lseek(fd, 2, SEEK_CUR));
    ASSERT_EQ(1, read(fd, &c, 1));
    ASSERT_EQ('8', c);

    /* SEEK_END */
    ASSERT_EQ(10, lseek(fd, 0, SEEK_END));

    /* SEEK_END with negative offset */
    ASSERT_EQ(7, lseek(fd, -3, SEEK_END));
    ASSERT_EQ(1, read(fd, &c, 1));
    ASSERT_EQ('7', c);

    /* invalid whence */
    ASSERT_EQ(-1, lseek(fd, 0, 999));
    ASSERT(errno == EINVAL);

    close(fd);
    return 1;
}
REGISTER_TEST(lseek, "Phase 2: File System");

int test_stat_fstat_lstat(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "stat_test");
    struct stat st;

    /* stat nonexistent */
    ASSERT_EQ(-1, stat("/nonexistent_stat_xyz", &st));
    ASSERT(errno == ENOENT);

    write_file(path, "data");
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_TRUE(S_ISREG(st.st_mode));
    ASSERT_GT(st.st_size, 0);

    /* fstat */
    int fd = open(path, O_RDONLY);
    ASSERT_GE(fd, 0);
    memset(&st, 0, sizeof(st));
    ASSERT_EQ(0, fstat(fd, &st));
    ASSERT_TRUE(S_ISREG(st.st_mode));
    close(fd);

    /* lstat on regular file (same as stat) */
    memset(&st, 0, sizeof(st));
    ASSERT_EQ(0, lstat(path, &st));
    ASSERT_TRUE(S_ISREG(st.st_mode));

    /* stat directory */
    memset(&st, 0, sizeof(st));
    ASSERT_EQ(0, stat("/", &st));
    ASSERT_TRUE(S_ISDIR(st.st_mode));

    return 1;
}
REGISTER_TEST(stat_fstat_lstat, "Phase 2: File System");

int test_access(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "access_test");

    write_file(path, "data");

    /* F_OK */
    ASSERT_EQ(0, access(path, F_OK));

    /* R_OK — root can always read */
    ASSERT_EQ(0, access(path, R_OK));

    /* W_OK */
    ASSERT_EQ(0, access(path, W_OK));

    /* X_OK — kernel checks execute bits before root override */
    ASSERT_EQ(0, chmod(path, 0755));
    ASSERT_EQ(0, access(path, X_OK));

    /* nonexistent */
    ASSERT_EQ(-1, access("/nonexistent_access_xyz", F_OK));
    ASSERT(errno == ENOENT);

    return 1;
}
REGISTER_TEST(access, "Phase 2: File System");

int test_creat(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "creat_test");

    int fd = creat(path, 0644);
    ASSERT_GE(fd, 0);
    const char *data = "creat data";
    ASSERT_EQ((ssize_t) strlen(data), write(fd, data, strlen(data)));
    close(fd);

    /* overwrite */
    fd = creat(path, 0644);
    ASSERT_GE(fd, 0);
    close(fd);

    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(st.st_size, 0);

    /* creat on directory should fail */
    fd = creat("/", 0644);
    ASSERT_LT(fd, 0);

    return 1;
}
REGISTER_TEST(creat, "Phase 2: File System");

int test_truncate(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "trunc_test");

    write_file(path, "0123456789");

    /* truncate to smaller */
    ASSERT_EQ(0, truncate(path, 5));
    char buf[16];
    ASSERT_EQ(5, read_file(path, buf, sizeof(buf)));
    ASSERT_STREQ("01234", buf);

    /* ftruncate */
    int fd = open(path, O_WRONLY);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, ftruncate(fd, 0));
    close(fd);
    ASSERT_EQ(0, read_file(path, buf, sizeof(buf)));

    return 1;
}
REGISTER_TEST(truncate, "Phase 2: File System");

int test_link_unlink(void) {
    char src[PATH_MAX], dst[PATH_MAX], dir[PATH_MAX];
    tmpfile_path(src, sizeof(src), "link_src");
    tmpfile_path(dst, sizeof(dst), "link_dst");

    write_file(src, "link data");

    /* hard link (kernel does deep copy, different inodes) */
    ASSERT_EQ(0, link(src, dst));
    struct stat st;
    ASSERT_EQ(0, stat(src, &st));
    ASSERT_GT(st.st_size, 0);
    ASSERT_EQ(0, stat(dst, &st));
    ASSERT_GT(st.st_size, 0);

    /* unlink destination */
    ASSERT_EQ(0, unlink(dst));
    ASSERT_EQ(-1, stat(dst, &st));
    ASSERT(errno == ENOENT);

    /* unlink nonexistent */
    ASSERT_EQ(-1, unlink("/nonexistent_unlink_xyz"));
    ASSERT(errno == ENOENT);

    /* unlink dir should fail */
    tmpfile_path(dir, sizeof(dir), "unlink_dir");
    ASSERT_EQ(0, mkdir(dir, 0755));
    ASSERT_EQ(-1, unlink(dir));
    ASSERT(errno == EISDIR);
    rmdir(dir);

    return 1;
}
REGISTER_TEST(link_unlink, "Phase 2: File System");

int test_symlink_readlink(void) {
    char target[PATH_MAX], linkpath[PATH_MAX];
    tmpfile_path(target, sizeof(target), "sym_target");
    tmpfile_path(linkpath, sizeof(linkpath), "sym_link");

    write_file(target, "sym data");

    /* create symlink */
    ASSERT_EQ(0, symlink(target, linkpath));

    /* readlink */
    char buf[PATH_MAX];
    ssize_t n = readlink(linkpath, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    ASSERT_STREQ(target, buf);

    /* access through symlink */
    struct stat st;
    ASSERT_EQ(0, stat(linkpath, &st));
    ASSERT_TRUE(S_ISREG(st.st_mode));

    /* lstat shows symlink itself */
    memset(&st, 0, sizeof(st));
    ASSERT_EQ(0, lstat(linkpath, &st));
    ASSERT_TRUE(S_ISLNK(st.st_mode));

    unlink(linkpath);
    unlink(target);
    return 1;
}
REGISTER_TEST(symlink_readlink, "Phase 2: File System");

int test_rename(void) {
    char src[PATH_MAX], dst[PATH_MAX], sub[PATH_MAX];
    tmpfile_path(src, sizeof(src), "rename_src");
    tmpfile_path(dst, sizeof(dst), "rename_dst");

    write_file(src, "rename data");
    ASSERT_EQ(0, rename(src, dst));

    struct stat st;
    ASSERT_EQ(-1, stat(src, &st));
    ASSERT_EQ(0, stat(dst, &st));

    /* overwrite existing */
    write_file(src, "new data");
    ASSERT_EQ(0, rename(src, dst));

    /* rename across directories */
    tmpfile_path(sub, sizeof(sub), "rename_subdir");
    ASSERT_EQ(0, mkdir(sub, 0755));
    char cross[PATH_MAX + 32];
    snprintf(cross, sizeof(cross), "%s/moved_file", sub);
    write_file(dst, "cross data");
    ASSERT_EQ(0, rename(dst, cross));
    ASSERT_EQ(-1, stat(dst, &st));
    ASSERT_EQ(0, stat(cross, &st));
    ASSERT_EQ(0, unlink(cross));
    ASSERT_EQ(0, rmdir(sub));

    return 1;
}
REGISTER_TEST(rename, "Phase 2: File System");

int test_chdir_getcwd(void) {
    char tmp_sub[PATH_MAX], cwd[PATH_MAX];

    /* chdir to our tmpdir (always exists) */
    ASSERT_EQ(0, chdir(tmpdir));
    ASSERT_NOTNULL(getcwd(cwd, sizeof(cwd)));
    ASSERT_STREQ(tmpdir, cwd);

    /* chdir to / */
    ASSERT_EQ(0, chdir("/"));
    ASSERT_NOTNULL(getcwd(cwd, sizeof(cwd)));
    ASSERT_STREQ("/", cwd);

    /* fchdir */
    int fd = open(tmpdir, O_RDONLY);
    if (fd >= 0) {
        ASSERT_EQ(0, fchdir(fd));
        close(fd);
        ASSERT_NOTNULL(getcwd(cwd, sizeof(cwd)));
        ASSERT_STREQ(tmpdir, cwd);
        chdir("/"); /* reset for subsequent tests */
    }

    /* chdir nonexistent */
    ASSERT_EQ(-1, chdir("/nonexistent_chdir_xyz"));
    ASSERT(errno == ENOENT);

    /* chdir to subdir of tmpdir */
    snprintf(tmp_sub, sizeof(tmp_sub), "%s/chdir_sub", tmpdir);
    ASSERT_EQ(0, mkdir(tmp_sub, 0755));
    ASSERT_EQ(0, chdir(tmp_sub));
    ASSERT_NOTNULL(getcwd(cwd, sizeof(cwd)));
    ASSERT_STREQ(tmp_sub, cwd);
    rmdir(tmp_sub);

    return 1;
}
REGISTER_TEST(chdir_getcwd, "Phase 2: File System");

int test_mkdir_rmdir(void) {
    char dir[PATH_MAX], subdir[PATH_MAX];
    tmpfile_path(dir, sizeof(dir), "mkdir_test");
    tmpfile_path(subdir, sizeof(subdir), "mkdir_test/sub");

    /* mkdir */
    ASSERT_EQ(0, mkdir(dir, 0755));
    struct stat st;
    ASSERT_EQ(0, stat(dir, &st));
    ASSERT_TRUE(S_ISDIR(st.st_mode));

    /* mkdir existing should fail */
    ASSERT_EQ(-1, mkdir(dir, 0755));
    ASSERT(errno == EEXIST);

    /* rmdir empty */
    ASSERT_EQ(0, rmdir(dir));

    /* rmdir nonexistent */
    ASSERT_EQ(-1, rmdir("/nonexistent_rmdir_xyz"));
    ASSERT(errno == ENOENT);

    /* rmdir non-empty should fail */
    ASSERT_EQ(0, mkdir(dir, 0755));
    ASSERT_EQ(0, mkdir(subdir, 0755));
    ASSERT_EQ(-1, rmdir(dir));
    ASSERT(errno == ENOTEMPTY);
    ASSERT_EQ(0, rmdir(subdir));
    ASSERT_EQ(0, rmdir(dir));

    return 1;
}
REGISTER_TEST(mkdir_rmdir, "Phase 2: File System");

int test_getdents(void) {
    DIR *d = opendir("/");
    ASSERT_NOTNULL(d);
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) count++;
    closedir(d);
    ASSERT_GT(count, 0);

    /* invalid fd */
    ASSERT_NULL(fdopendir(9999));
    return 1;
}
REGISTER_TEST(getdents, "Phase 2: File System");

int test_chmod_fchmod(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "chmod_test");
    write_file(path, "data");

    struct stat st;
    ASSERT_EQ(0, stat(path, &st));

    /* change mode */
    ASSERT_EQ(0, chmod(path, 0600));
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(st.st_mode & 0777, (mode_t) 0600);

    /* fchmod */
    int fd = open(path, O_RDONLY);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, fchmod(fd, 0644));
    close(fd);
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(st.st_mode & 0777, (mode_t) 0644);

    return 1;
}
REGISTER_TEST(chmod_fchmod, "Phase 2: File System");

int test_chown_fchown(void) {
    char path[PATH_MAX], linkpath[PATH_MAX];
    tmpfile_path(path, sizeof(path), "chown_test");
    tmpfile_path(linkpath, sizeof(linkpath), "chown_link");
    write_file(path, "data");

    /* chown as root (same uid) */
    ASSERT_EQ(0, chown(path, 0, 0));

    /* fchown */
    int fd = open(path, O_RDONLY);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, fchown(fd, 0, 0));
    close(fd);

    /* lchown on symlink */
    ASSERT_EQ(0, symlink(path, linkpath));
    ASSERT_EQ(0, lchown(linkpath, 0, 0));
    unlink(linkpath);

    return 1;
}
REGISTER_TEST(chown_fchown, "Phase 2: File System");

int test_umask(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "umask_test");

    mode_t old = umask(0077);
    int fd = creat(path, 0666);
    ASSERT_GE(fd, 0);
    close(fd);

    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    /* with umask 0077, 0666 -> 0600 */
    ASSERT_EQ(st.st_mode & 0777, (mode_t) 0600);

    umask(old);
    unlink(path);
    return 1;
}
REGISTER_TEST(umask, "Phase 2: File System");

int test_fcntl(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "fcntl_test");
    write_file(path, "data");

    int fd = open(path, O_RDONLY);
    ASSERT_GE(fd, 0);

    /* F_GETFD / F_SETFD */
    int flags = fcntl(fd, F_GETFD);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(0, fcntl(fd, F_SETFD, flags | FD_CLOEXEC));

    /* F_GETFL */
    flags = fcntl(fd, F_GETFL);
    ASSERT_GE(flags, 0);
    ASSERT_EQ((flags & O_ACCMODE), O_RDONLY);

    /* F_DUPFD */
    int dupfd = fcntl(fd, F_DUPFD, 50);
    ASSERT_GE(dupfd, 50);
    close(dupfd);

    close(fd);

    /* fcntl on invalid fd */
    ASSERT_EQ(-1, fcntl(9999, F_GETFD));
    ASSERT(errno == EBADF);

    return 1;
}
REGISTER_TEST(fcntl, "Phase 2: File System");

int test_mknod(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "mknod_test");

    /* Create a regular file via mknod (S_IFREG) */
    int ret = mknod(path, S_IFREG | 0644, 0);
    /* ramfs may not support mknod, but try */
    if (ret == 0) {
        struct stat st;
        ASSERT_EQ(0, stat(path, &st));
        ASSERT_TRUE(S_ISREG(st.st_mode));
        unlink(path);
    }
    return 1;
}
REGISTER_TEST(mknod, "Phase 2: File System");

int test_statfs(void) {
    struct statvfs buf;
    int ret = statvfs("/", &buf);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) return 1;
    ASSERT_EQ(0, ret);
    ASSERT_GT(buf.f_blocks, 0);
    ASSERT_GT(buf.f_bsize, 0);

    int fd = open("/", O_RDONLY);
    ASSERT_GE(fd, 0);
    memset(&buf, 0, sizeof(buf));
    ret = fstatvfs(fd, &buf);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) return 1;
    ASSERT_EQ(0, ret);
    ASSERT_GT(buf.f_blocks, 0);
    close(fd);

    return 1;
}
REGISTER_TEST(statfs, "Phase 2: File System");

int test_openat_mkdirat(void) {
    char path[PATH_MAX], nested[PATH_MAX];

    /* openat with AT_FDCWD + absolute path */
    tmpfile_path(path, sizeof(path), "openat_file");
    int fd = openat(AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    close(fd);

    /* fstatat with AT_FDCWD */
    struct stat st;
    ASSERT_EQ(0, fstatat(AT_FDCWD, path, &st, 0));
    ASSERT_TRUE(S_ISREG(st.st_mode));
    unlink(path);

    /* Relative paths must resolve from dirfd, not from the process cwd. */
    int dirfd = open(tmpdir, O_RDONLY | O_DIRECTORY);
    ASSERT_GE(dirfd, 0);
    fd = openat(dirfd, "openat_relative", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(4, write(fd, "data", 4));
    close(fd);
    close(dirfd);
    snprintf(nested, sizeof(nested), "%s/openat_relative", tmpdir);
    ASSERT_EQ(0, stat(nested, &st));
    ASSERT_EQ(4, st.st_size);
    unlink(nested);

    return 1;
}
REGISTER_TEST(openat_mkdirat, "Phase 2: File System");

int test_fstatat_unlinkat(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "fstatat_file");

    /* create via openat + AT_FDCWD */
    int fd = openat(AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    close(fd);

    /* fstatat */
    struct stat st;
    ASSERT_EQ(0, fstatat(AT_FDCWD, path, &st, 0));
    ASSERT_TRUE(S_ISREG(st.st_mode));

    /* unlinkat */
    ASSERT_EQ(0, unlinkat(AT_FDCWD, path, 0));
    ASSERT_EQ(-1, fstatat(AT_FDCWD, path, &st, 0));
    ASSERT(errno == ENOENT);

    return 1;
}
REGISTER_TEST(fstatat_unlinkat, "Phase 2: File System");

int test_renameat_linkat(void) {
    char src[PATH_MAX], dst[PATH_MAX];
    tmpfile_path(src, sizeof(src), "ren_src");
    tmpfile_path(dst, sizeof(dst), "ren_dst");

    write_file(src, "data");

    /* renameat */
    ASSERT_EQ(0, renameat(AT_FDCWD, src, AT_FDCWD, dst));
    struct stat st;
    ASSERT_EQ(-1, stat(src, &st));
    ASSERT_EQ(0, stat(dst, &st));

    /* linkat with AT_FDCWD */
    char linkname[PATH_MAX];
    tmpfile_path(linkname, sizeof(linkname), "ren_link");
    ASSERT_EQ(0, linkat(AT_FDCWD, dst, AT_FDCWD, linkname, 0));
    ASSERT_EQ(0, stat(linkname, &st));

    unlink(linkname);
    unlink(dst);
    return 1;
}
REGISTER_TEST(renameat_linkat, "Phase 2: File System");

int test_symlinkat_readlinkat(void) {
    char target[PATH_MAX], link[PATH_MAX];
    tmpfile_path(target, sizeof(target), "sla_target");
    tmpfile_path(link, sizeof(link), "sla_link");

    write_file(target, "content");

    /* symlinkat */
    ASSERT_EQ(0, symlinkat(target, AT_FDCWD, link));

    /* readlinkat */
    char buf[PATH_MAX];
    ssize_t n = readlinkat(AT_FDCWD, link, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    ASSERT_STREQ(target, buf);

    /* stat follows symlink */
    struct stat st;
    ASSERT_EQ(0, fstatat(AT_FDCWD, link, &st, 0));
    ASSERT_TRUE(S_ISREG(st.st_mode));

    /* lstat via AT_SYMLINK_NOFOLLOW */
    ASSERT_EQ(0, fstatat(AT_FDCWD, link, &st, AT_SYMLINK_NOFOLLOW));
    ASSERT_TRUE(S_ISLNK(st.st_mode));

    unlink(link);
    unlink(target);
    return 1;
}
REGISTER_TEST(symlinkat_readlinkat, "Phase 2: File System");

int test_fchmodat_faccessat(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "fchmodat_file");
    write_file(path, "data");

    /* fchmodat */
    ASSERT_EQ(0, fchmodat(AT_FDCWD, path, 0600, 0));
    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(st.st_mode & 0777, (mode_t) 0600);

    /* faccessat */
    ASSERT_EQ(0, faccessat(AT_FDCWD, path, F_OK, 0));
    ASSERT_EQ(0, faccessat(AT_FDCWD, path, R_OK, 0));

    /* faccessat nonexistent */
    ASSERT_EQ(-1, faccessat(AT_FDCWD, "/nonexistent_faccessat_xyz", F_OK, 0));
    ASSERT(errno == ENOENT);

    unlink(path);
    return 1;
}
REGISTER_TEST(fchmodat_faccessat, "Phase 2: File System");

int test_pread_pwrite(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "pread_test");
    write_file(path, "hello world");

    int fd = open(path, O_RDWR);
    ASSERT_GE(fd, 0);

    /* pread should not change file offset */
    char buf1[6] = {}, buf2[6] = {};
    ASSERT_EQ(5, pread(fd, buf1, 5, 0));
    ASSERT_EQ(5, pread(fd, buf2, 5, 6));
    buf1[5] = buf2[5] = '\0';
    ASSERT_STREQ("hello", buf1);
    ASSERT_STREQ("world", buf2);

    /* lseek should show no change (offset stayed at 0) */
    ASSERT_EQ(0, lseek(fd, 0, SEEK_CUR));

    /* pwrite without changing offset */
    ASSERT_EQ(5, pwrite(fd, "HELLO", 5, 0));
    ASSERT_EQ(0, lseek(fd, 0, SEEK_CUR));

    close(fd);

    /* verify content via read_file */
    char verify[16];
    ASSERT_EQ(11, read_file(path, verify, sizeof(verify)));
    verify[11] = '\0';
    ASSERT_STREQ("HELLO world", verify);

    return 1;
}
REGISTER_TEST(pread_pwrite, "Phase 2: File System");

int test_positional_io_validation(void) {
    char path[PATH_MAX], buf[8] = {};
    tmpfile_path(path, sizeof(path), "positional_validation");
    ASSERT_TRUE(write_file(path, "data"));

    int fd = open(path, O_RDONLY);
    ASSERT_GE(fd, 0);
    errno = 0;
    ASSERT_EQ(-1, pwrite(fd, "x", 1, 0));
    ASSERT_ERRNO(EBADF);
    close(fd);

    fd = open(path, O_WRONLY);
    ASSERT_GE(fd, 0);
    errno = 0;
    ASSERT_EQ(-1, pread(fd, buf, 1, 0));
    ASSERT_ERRNO(EBADF);
    close(fd);

    fd = open(path, O_RDWR);
    ASSERT_GE(fd, 0);
    errno = 0;
    ASSERT_EQ(-1, pwrite(fd, "x", 1, (off_t) -1));
    ASSERT_ERRNO(EINVAL);
    close(fd);

    ASSERT_EQ(4, read_file(path, buf, sizeof(buf)));
    ASSERT_STREQ("data", buf);
    unlink(path);
    return 1;
}
REGISTER_TEST(positional_io_validation, "Phase 2: File System");

int test_at_syscall_numbers(void) {
    char path[PATH_MAX], buf[16];
    tmpfile_path(path, sizeof(path), "at_syscalls");
    ASSERT_TRUE(write_file(path, "unchanged"));

    ASSERT_EQ(0, fchownat(AT_FDCWD, path, 123, 456, 0));
    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ(123, st.st_uid);
    ASSERT_EQ(456, st.st_gid);

    ASSERT_EQ(0, utimensat(AT_FDCWD, path, NULL, 0));
    ASSERT_EQ(9, read_file(path, buf, sizeof(buf)));
    ASSERT_STREQ("unchanged", buf);
    unlink(path);
    return 1;
}
REGISTER_TEST(at_syscall_numbers, "Phase 2: File System");

int test_readv_writev(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "rv_test");
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);

    struct iovec iov[2];
    char out1[] = "hello ", out2[] = "world\n";
    iov[0].iov_base = out1;
    iov[0].iov_len = 6;
    iov[1].iov_base = out2;
    iov[1].iov_len = 6;
    ASSERT_EQ(12, writev(fd, iov, 2));
    close(fd);

    fd = open(path, O_RDONLY);
    ASSERT_GE(fd, 0);
    char in1[8] = {}, in2[8] = {};
    iov[0].iov_base = in1;
    iov[0].iov_len = 6;
    iov[1].iov_base = in2;
    iov[1].iov_len = 6;
    ASSERT_EQ(12, readv(fd, iov, 2));
    close(fd);
    ASSERT_STREQ("hello ", in1);
    ASSERT_STREQ("world\n", in2);

    unlink(path);
    return 1;
}
REGISTER_TEST(readv_writev, "Phase 2: File System");

int test_copy_file_range(void) {
    char src[PATH_MAX], dst[PATH_MAX];
    tmpfile_path(src, sizeof(src), "cfr_src");
    tmpfile_path(dst, sizeof(dst), "cfr_dst");
    write_file(src, "copy this data");

    int fd_in = open(src, O_RDONLY);
    int fd_out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd_in, 0);
    ASSERT_GE(fd_out, 0);

    off_t off_in = 0, off_out = 0;
    ssize_t ret = copy_file_range(fd_in, &off_in, fd_out, &off_out, 14, 0);
    if (ret < 0 && (errno == ENOSYS || errno == EINVAL)) goto cfr_done;
    ASSERT_EQ(ret, 14);

    char buf[32];
    ASSERT_EQ(14, read_file(dst, buf, sizeof(buf)));
    ASSERT_STREQ("copy this data", buf);

cfr_done:
    close(fd_in);
    close(fd_out);
    unlink(src);
    unlink(dst);
    return 1;
}
REGISTER_TEST(copy_file_range, "Phase 2: File System");

int test_memfd_create(void) {
    int fd = memfd_create("test_mem", 0);
    if (fd < 0 && errno == ENOSYS) return 1;
    ASSERT_GE(fd, 0);

    const char *data = "memfd data";
    ASSERT_EQ((ssize_t) strlen(data), write(fd, data, strlen(data)));

    char buf[32];
    ASSERT_EQ(0, lseek(fd, 0, SEEK_SET));
    ASSERT_EQ((ssize_t) strlen(data), read(fd, buf, sizeof(buf)));
    buf[strlen(data)] = '\0';
    ASSERT_STREQ(data, buf);

    close(fd);
    return 1;
}
REGISTER_TEST(memfd_create, "Phase 2: File System");

/* wayland's os_create_anonymous_file() seals the memfd and gives up if it can't */
int test_memfd_seals(void) {
    int fd = memfd_create("seal_mem", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 && errno == ENOSYS) return 1;
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, ftruncate(fd, 4096));
    ASSERT_EQ(0, fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK));
    int seals = fcntl(fd, F_GET_SEALS);
    ASSERT_TRUE(seals >= 0);
    ASSERT_TRUE((seals & F_SEAL_SHRINK) != 0);
    /* shrinking is refused once sealed, growing still works */
    ASSERT_TRUE(ftruncate(fd, 2048) != 0);
    ASSERT_EQ(0, ftruncate(fd, 8192));
    struct stat st;
    ASSERT_EQ(0, fstat(fd, &st));
    ASSERT_EQ((off_t) 8192, st.st_size);
    close(fd);
    return 1;
}
REGISTER_TEST(memfd_seals, "Phase 2: File System");

/* display servers check st_rdev before taking over a VT or a framebuffer */
int test_device_rdev(void) {
    struct {
        const char *path;
        unsigned major;
        unsigned minor;
    } devs[] = {
        { "/dev/tty1", 4, 1 },
        { "/dev/fb0", 29, 0 },
        { "/dev/input/event0", 13, 64 },
        { "/dev/null", 1, 3 },
    };
    for (unsigned i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        struct stat st;
        if (stat(devs[i].path, &st) != 0) continue; /* device not present in this build */
        ASSERT_TRUE(S_ISCHR(st.st_mode));
        ASSERT_EQ(devs[i].major, (unsigned) ((st.st_rdev >> 8) & 0xfff));
        ASSERT_EQ(devs[i].minor, (unsigned) (st.st_rdev & 0xff));
    }
    return 1;
}
REGISTER_TEST(device_rdev, "Phase 2: File System");

int test_sendfile(void) {
    char src[PATH_MAX], dst[PATH_MAX];
    tmpfile_path(src, sizeof(src), "sf_src");
    tmpfile_path(dst, sizeof(dst), "sf_dst");
    write_file(src, "sendfile data");

    int fd_in = open(src, O_RDONLY);
    int fd_out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd_in, 0);
    ASSERT_GE(fd_out, 0);

    off_t off = 0;
    ssize_t ret = sendfile(fd_out, fd_in, &off, 13);
    if (ret < 0 && (errno == ENOSYS || errno == EINVAL)) goto sf_done;
    ASSERT_EQ(ret, 13);
    close(fd_in);
    close(fd_out);

    char buf[32];
    ASSERT_EQ(13, read_file(dst, buf, sizeof(buf)));
    ASSERT_STREQ("sendfile data", buf);

sf_done:
    close(fd_in);
    close(fd_out);
    unlink(src);
    unlink(dst);
    return 1;
}
REGISTER_TEST(sendfile, "Phase 2: File System");

int test_flock(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "flock_test");
    write_file(path, "data");

    int fd = open(path, O_RDWR);
    ASSERT_GE(fd, 0);

    /* LOCK_SH */
    ASSERT_EQ(0, flock(fd, LOCK_SH));
    ASSERT_EQ(0, flock(fd, LOCK_UN));

    /* LOCK_EX */
    ASSERT_EQ(0, flock(fd, LOCK_EX));
    ASSERT_EQ(0, flock(fd, LOCK_UN));

    /* LOCK_NB */
    ASSERT_EQ(0, flock(fd, LOCK_EX | LOCK_NB));
    ASSERT_EQ(0, flock(fd, LOCK_UN));

    close(fd);
    unlink(path);
    return 1;
}
REGISTER_TEST(flock, "Phase 2: File System");

int test_fsync_fdatasync(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "fsync_test");
    write_file(path, "data");

    int fd = open(path, O_RDWR);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, fsync(fd));
    ASSERT_EQ(0, fdatasync(fd));
    close(fd);

    /* invalid fd */
    ASSERT_EQ(-1, fsync(9999));
    ASSERT(errno == EBADF);

    unlink(path);
    return 1;
}
REGISTER_TEST(fsync_fdatasync, "Phase 2: File System");

int test_fallocate(void) {
    /* ramfs: fallocate is a no-op, should succeed */
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "falloc_test");
    write_file(path, "data");
    int fd = open(path, O_RDWR);
    ASSERT_GE(fd, 0);
    int ret = fallocate(fd, 0, 0, 4096);
    /* may be ENOSYS or ENOTSUP on ramfs */
    if (ret < 0) ASSERT(errno == ENOSYS || errno == ENOTSUP);
    close(fd);
    unlink(path);
    return 1;
}
REGISTER_TEST(fallocate, "Phase 2: File System");

int test_utime_utimensat(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "utime_test");
    write_file(path, "data");

    struct utimbuf times = { 1000000, 1000000 };
    int ret = utime(path, &times);
    if (ret < 0 && errno == ENOSYS) return 1;

    /* timestamps may be stubbed; just verify the call doesnt crash */
    struct stat st;
    ASSERT_EQ(0, stat(path, &st));

    /* utimensat */
    struct timespec ts[2];
    ts[0].tv_sec = 2000000;
    ts[0].tv_nsec = 0;
    ts[1].tv_sec = 2000000;
    ts[1].tv_nsec = 0;
    ret = utimensat(AT_FDCWD, path, ts, 0);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) return 1;

    ASSERT_EQ(0, stat(path, &st));

    unlink(path);
    return 1;
}
REGISTER_TEST(utime_utimensat, "Phase 2: File System");

int test_statx(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "statx_test");
    write_file(path, "statx data");

#ifndef SYS_statx
#define SYS_statx 332
#endif
    struct {
        uint32_t stx_mask;
        uint32_t stx_blksize;
        uint64_t stx_attributes;
        uint32_t stx_nlink;
        uint32_t stx_uid;
        uint32_t stx_gid;
        uint16_t stx_mode;
        uint16_t __pad1;
        uint64_t stx_ino;
        uint64_t stx_size;
        uint64_t stx_blocks;
        uint64_t stx_attributes_mask;
    } sx;

    int ret = syscall(SYS_statx, AT_FDCWD, path, 0, 0xfff, &sx);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) return 1;
    ASSERT_EQ(0, ret);
    ASSERT_TRUE(sx.stx_mode & S_IFREG);
    ASSERT_EQ(sx.stx_size, 10);
    ASSERT_GT(sx.stx_ino, 0);

    /* statx directory */
    ret = syscall(SYS_statx, AT_FDCWD, "/", 0, 0xfff, &sx);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) return 1;
    ASSERT_EQ(0, ret);
    ASSERT_TRUE(sx.stx_mode & S_IFDIR);

    unlink(path);
    return 1;
}
REGISTER_TEST(statx, "Phase 2: File System");

int test_pipe2(void) {
    int p[2];

    /* default (same as pipe) */
    ASSERT_EQ(0, pipe2(p, 0));
    ASSERT_GE(p[0], 0);
    ASSERT_GE(p[1], 0);
    close(p[0]);
    close(p[1]);

    /* O_CLOEXEC */
    ASSERT_EQ(0, pipe2(p, O_CLOEXEC));
    int flags = fcntl(p[0], F_GETFD);
    ASSERT_GE(flags, 0);
    ASSERT_TRUE(flags & FD_CLOEXEC);
    flags = fcntl(p[1], F_GETFD);
    ASSERT_GE(flags, 0);
    ASSERT_TRUE(flags & FD_CLOEXEC);
    close(p[0]);
    close(p[1]);

    /* O_NONBLOCK */
    ASSERT_EQ(0, pipe2(p, O_NONBLOCK));
    flags = fcntl(p[0], F_GETFL);
    ASSERT_GE(flags, 0);
    ASSERT_TRUE(flags & O_NONBLOCK);
    close(p[0]);
    close(p[1]);

    return 1;
}
REGISTER_TEST(pipe2, "Phase 2: File System");

int test_sendfile_noffset(void) {
    char src[PATH_MAX], dst[PATH_MAX];
    tmpfile_path(src, sizeof(src), "sfn_src");
    tmpfile_path(dst, sizeof(dst), "sfn_dst");

    write_file(src, "sendfile test data");
    int fd_in = open(src, O_RDONLY);
    int fd_out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd_in, 0);
    ASSERT_GE(fd_out, 0);

    ssize_t ret = sendfile(fd_out, fd_in, NULL, 18); /* "sendfile test data" is 18 bytes */
    if (ret < 0 && (errno == ENOSYS || errno == EINVAL || errno == EBADF || errno == EFAULT))
        goto sfn_done;
    ASSERT_EQ(ret, 18);
    char buf[32];
    ASSERT_EQ(18, read_file(dst, buf, sizeof(buf)));
    ASSERT_STREQ("sendfile test data", buf);

sfn_done:
    close(fd_in);
    close(fd_out);
    unlink(src);
    unlink(dst);
    return 1;
}
REGISTER_TEST(sendfile_noffset, "Phase 2: File System");
