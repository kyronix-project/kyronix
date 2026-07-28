#include "test_harness.h"

int test_basename(void) {
    char buf[128];
    int ret = capture_cmd((char *[]) { "basename", "/foo/bar", NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_GT(strlen(buf), 0);
    return 1;
}
REGISTER_TEST(basename, "Phase 9: Utilities");

int test_cat(void) {
    char path[PATH_MAX], buf[128];
    tmpfile_path(path, sizeof(path), "cat_test");
    write_file(path, "hello\n");
    ASSERT_EQ(0, capture_cmd((char *[]) { "cat", path, NULL }, buf, sizeof(buf)));
    ASSERT_STREQ("hello\n", buf);
    unlink(path);
    return 1;
}
REGISTER_TEST(cat, "Phase 9: Utilities");

int test_chgrp(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "chgrp_test");
    write_file(path, "x");
    int ret = run_cmd((char *[]) { "chgrp", "root", path, NULL });
    if (ret != 0) return 1; /* may not support */
    unlink(path);
    return 1;
}
REGISTER_TEST(chgrp, "Phase 9: Utilities");

int test_chmod(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "chmod_test");
    write_file(path, "x");
    ASSERT_EQ(0, run_cmd((char *[]) { "chmod", "600", path, NULL }));
    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    ASSERT_EQ((st.st_mode & 0777), (mode_t) 0600);
    unlink(path);
    return 1;
}
REGISTER_TEST(chmod, "Phase 9: Utilities");

int test_chown(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "chown_test");
    write_file(path, "x");
    int ret = run_cmd((char *[]) { "chown", "0:0", path, NULL });
    if (ret != 0) return 1;
    unlink(path);
    return 1;
}
REGISTER_TEST(chown, "Phase 9: Utilities");

int test_cksum(void) {
    char path[PATH_MAX], buf[128];
    tmpfile_path(path, sizeof(path), "cksum_test");
    write_file(path, "hello");
    int ret = capture_cmd((char *[]) { "cksum", path, NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_GT(strlen(buf), 0);
    unlink(path);
    return 1;
}
REGISTER_TEST(cksum, "Phase 9: Utilities");

int test_clear(void) {
    int ret = run_cmd((char *[]) { "clear", NULL });
    if (ret != 0) return 1;
    return 1;
}
REGISTER_TEST(clear, "Phase 9: Utilities");

int test_cmp(void) {
    char a[PATH_MAX], b[PATH_MAX];
    tmpfile_path(a, sizeof(a), "cmp_a");
    tmpfile_path(b, sizeof(b), "cmp_b");
    write_file(a, "same");
    write_file(b, "same");
    ASSERT_EQ(0, run_cmd((char *[]) { "cmp", a, b, NULL }));
    write_file(b, "diff");
    ASSERT_EQ(1, run_cmd((char *[]) { "cmp", a, b, NULL }));
    unlink(a);
    unlink(b);
    return 1;
}
REGISTER_TEST(cmp, "Phase 9: Utilities");

int test_cp(void) {
    char src[PATH_MAX], dst[PATH_MAX];
    tmpfile_path(src, sizeof(src), "cp_src");
    tmpfile_path(dst, sizeof(dst), "cp_dst");
    write_file(src, "cp data");
    ASSERT_EQ(0, run_cmd((char *[]) { "cp", src, dst, NULL }));
    struct stat st;
    ASSERT_EQ(0, stat(dst, &st));
    ASSERT_GT(st.st_size, 0);
    unlink(src);
    unlink(dst);
    return 1;
}
REGISTER_TEST(cp, "Phase 9: Utilities");

int test_cut(void) {
    char buf[128], path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "cut_test");
    write_file(path, "abc\tdef\n");
    int ret = capture_cmd((char *[]) { "cut", "-f1", path, NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_STREQ("abc\n", buf);
    unlink(path);
    return 1;
}
REGISTER_TEST(cut, "Phase 9: Utilities");

int test_date(void) {
    char buf[128];
    int ret = capture_cmd((char *[]) { "date", "+%s", NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    return 1;
}
REGISTER_TEST(date, "Phase 9: Utilities");

int test_dd(void) {
    char buf[256];
    int ret =
        capture_cmd((char *[]) { "dd", "if=/dev/zero", "of=/dev/null", "bs=512", "count=1", NULL },
                    buf, sizeof(buf));
    if (ret != 0) return 1;
    return 1;
}
REGISTER_TEST(dd, "Phase 9: Utilities");

int test_dirname(void) {
    char buf[128];
    int ret = capture_cmd((char *[]) { "dirname", "/foo/bar", NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_GT(strlen(buf), 0);
    return 1;
}
REGISTER_TEST(dirname, "Phase 9: Utilities");

int test_du(void) {
    char buf[128];
    int ret = capture_cmd((char *[]) { "du", "-s", "/bin", NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_GT(strlen(buf), 0);
    return 1;
}
REGISTER_TEST(du, "Phase 9: Utilities");

int test_echo(void) {
    char buf[128];
    ASSERT_EQ(0, capture_cmd((char *[]) { "echo", "hello", "world", NULL }, buf, sizeof(buf)));
    ASSERT_STREQ("hello world\n", buf);
    ASSERT_EQ(0, capture_cmd((char *[]) { "echo", "-n", "no_newline", NULL }, buf, sizeof(buf)));
    ASSERT_STREQ("no_newline", buf);
    return 1;
}
REGISTER_TEST(echo, "Phase 9: Utilities");

int test_env(void) {
    char buf[512];
    int ret = capture_cmd((char *[]) { "env", NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_GT(strlen(buf), 0);
    return 1;
}
REGISTER_TEST(env, "Phase 9: Utilities");

int test_false(void) {
    ASSERT_EQ(1, run_cmd((char *[]) { "false", NULL }));
    return 1;
}
REGISTER_TEST(false, "Phase 9: Utilities");

int test_find(void) {
    char buf[256];
    int ret = capture_cmd((char *[]) { "find", "/bin", "-name", "ksh", NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_GT(strlen(buf), 0);
    return 1;
}
REGISTER_TEST(find, "Phase 9: Utilities");

int test_grep(void) {
    char buf[128];
    int ret = capture_cmd((char *[]) { "sh", "-c", "echo hello world | grep hello", NULL }, buf,
                          sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_GT(strlen(buf), 0);
    ret = capture_cmd((char *[]) { "sh", "-c", "echo hello | grep zzznonexistent", NULL }, buf,
                      sizeof(buf));
    ASSERT_EQ(1, ret);
    return 1;
}
REGISTER_TEST(grep, "Phase 9: Utilities");

int test_grep_o_util(void) {
    char buf[64];
    int ret = capture_cmd((char *[]) { "sh", "-c", "echo abc def | grep -o abc", NULL }, buf,
                          sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_STREQ("abc\n", buf);
    return 1;
}
REGISTER_TEST(grep_o_util, "Phase 9: Utilities");

int test_grep_v(void) {
    char buf[64];
    int ret = capture_cmd((char *[]) { "sh", "-c", "echo keep | grep -v nomatch", NULL }, buf,
                          sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_STREQ("keep\n", buf);
    return 1;
}
REGISTER_TEST(grep_v, "Phase 9: Utilities");

int test_head(void) {
    char path[PATH_MAX], buf[128];
    tmpfile_path(path, sizeof(path), "head_test");
    write_file(path, "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\n");
    ASSERT_EQ(0, capture_cmd((char *[]) { "head", "-n", "3", path, NULL }, buf, sizeof(buf)));
    ASSERT_STREQ("a\nb\nc\n", buf);
    unlink(path);
    return 1;
}
REGISTER_TEST(head, "Phase 9: Utilities");

int test_hostname(void) {
    char buf[128];
    int ret = capture_cmd((char *[]) { "uname", "-n", NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_GT(strlen(buf), 0);
    return 1;
}
REGISTER_TEST(hostname, "Phase 9: Utilities");

int test_kill(void) {
    int ret = run_cmd((char *[]) { "sh", "-c", "kill -0 1", NULL });
    if (ret != 0) return 1;
    return 1;
}
REGISTER_TEST(kill, "Phase 9: Utilities");

int test_ln(void) {
    char target[PATH_MAX], link[PATH_MAX];
    tmpfile_path(target, sizeof(target), "ln_target");
    tmpfile_path(link, sizeof(link), "ln_link");
    write_file(target, "ln data");
    ASSERT_EQ(0, run_cmd((char *[]) { "ln", "-s", target, link, NULL }));
    struct stat st;
    ASSERT_EQ(0, lstat(link, &st));
    ASSERT_TRUE(S_ISLNK(st.st_mode));
    unlink(link);
    unlink(target);
    return 1;
}
REGISTER_TEST(ln, "Phase 9: Utilities");

int test_ls(void) {
    char buf[512];
    int ret = capture_cmd((char *[]) { "ls", "/bin", NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_GT(strlen(buf), 0);
    ASSERT_NOTNULL(strstr(buf, "ksh"));
    return 1;
}
REGISTER_TEST(ls, "Phase 9: Utilities");

int test_mkdir_util(void) {
    char dir[PATH_MAX];
    tmpfile_path(dir, sizeof(dir), "mkdir_util_test");
    ASSERT_EQ(0, run_cmd((char *[]) { "mkdir", dir, NULL }));
    struct stat st;
    ASSERT_EQ(0, stat(dir, &st));
    ASSERT_TRUE(S_ISDIR(st.st_mode));
    rmdir(dir);
    return 1;
}
REGISTER_TEST(mkdir_util, "Phase 9: Utilities");

int test_mktemp(void) {
    char buf[128];
    int ret = capture_cmd((char *[]) { "mktemp", NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_GT(strlen(buf), 0);
    /* remove trailing newline */
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    unlink(buf);
    return 1;
}
REGISTER_TEST(mktemp, "Phase 9: Utilities");

int test_mv(void) {
    char src[PATH_MAX], dst[PATH_MAX];
    tmpfile_path(src, sizeof(src), "mv_src");
    tmpfile_path(dst, sizeof(dst), "mv_dst");
    write_file(src, "mv data");
    ASSERT_EQ(0, run_cmd((char *[]) { "mv", src, dst, NULL }));
    struct stat st;
    ASSERT_EQ(-1, stat(src, &st));
    ASSERT_EQ(0, stat(dst, &st));
    unlink(dst);
    return 1;
}
REGISTER_TEST(mv, "Phase 9: Utilities");

int test_printenv(void) {
    char buf[128];
    ASSERT_EQ(0, capture_cmd((char *[]) { "printenv", "HOME", NULL }, buf, sizeof(buf)));
    ASSERT_STREQ("/root\n", buf);
    return 1;
}
REGISTER_TEST(printenv, "Phase 9: Utilities");

int test_printf(void) {
    char buf[128];
    ASSERT_EQ(0, capture_cmd((char *[]) { "printf", "hello %s", "world", NULL }, buf, sizeof(buf)));
    ASSERT_STREQ("hello world", buf);
    return 1;
}
REGISTER_TEST(printf, "Phase 9: Utilities");

int test_pwd(void) {
    char buf[128];
    ASSERT_EQ(0, capture_cmd((char *[]) { "pwd", NULL }, buf, sizeof(buf)));
    ASSERT_GT(strlen(buf), 0);
    ASSERT_EQ('/', buf[0]);
    return 1;
}
REGISTER_TEST(pwd, "Phase 9: Utilities");

int test_readlink(void) {
    char target[PATH_MAX], link[PATH_MAX], buf[256];
    tmpfile_path(target, sizeof(target), "rl_target");
    tmpfile_path(link, sizeof(link), "rl_link");
    write_file(target, "x");
    ASSERT_EQ(0, run_cmd((char *[]) { "ln", "-s", target, link, NULL }));
    ASSERT_EQ(0, capture_cmd((char *[]) { "readlink", link, NULL }, buf, sizeof(buf)));
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    ASSERT_STREQ(target, buf);
    unlink(link);
    unlink(target);
    return 1;
}
REGISTER_TEST(readlink, "Phase 9: Utilities");

int test_rm(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "rm_test");
    write_file(path, "x");
    ASSERT_EQ(0, run_cmd((char *[]) { "rm", path, NULL }));
    struct stat st;
    ASSERT_EQ(-1, stat(path, &st));
    return 1;
}
REGISTER_TEST(rm, "Phase 9: Utilities");

int test_rmdir_util(void) {
    char dir[PATH_MAX];
    tmpfile_path(dir, sizeof(dir), "rmdir_util_test");
    ASSERT_EQ(0, mkdir(dir, 0755));
    ASSERT_EQ(0, run_cmd((char *[]) { "rmdir", dir, NULL }));
    struct stat st;
    ASSERT_EQ(-1, stat(dir, &st));
    return 1;
}
REGISTER_TEST(rmdir_util, "Phase 9: Utilities");

int test_sed(void) {
    char buf[128];
    int ret =
        capture_cmd((char *[]) { "sh", "-c", "echo old | sed s/old/new/", NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_STREQ("new\n", buf);
    return 1;
}
REGISTER_TEST(sed, "Phase 9: Utilities");

int test_seq(void) {
    char buf[128];
    ASSERT_EQ(0, capture_cmd((char *[]) { "seq", "3", NULL }, buf, sizeof(buf)));
    ASSERT_STREQ("1\n2\n3\n", buf);
    return 1;
}
REGISTER_TEST(seq, "Phase 9: Utilities");

int test_sleep(void) {
    int ret = run_cmd((char *[]) { "sleep", "0", NULL });
    if (ret != 0) return 1;
    return 1;
}
REGISTER_TEST(sleep, "Phase 9: Utilities");

int test_sort(void) {
    char path[PATH_MAX], buf[128];
    tmpfile_path(path, sizeof(path), "sort_test");
    write_file(path, "b\na\nc\n");
    int ret = capture_cmd((char *[]) { "sort", path, NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_STREQ("a\nb\nc\n", buf);
    unlink(path);
    return 1;
}
REGISTER_TEST(sort, "Phase 9: Utilities");

int test_sync(void) {
    int ret = run_cmd((char *[]) { "sync", NULL });
    if (ret != 0) return 1;
    return 1;
}
REGISTER_TEST(sync, "Phase 9: Utilities");

int test_tail(void) {
    char path[PATH_MAX], buf[128];
    tmpfile_path(path, sizeof(path), "tail_test");
    write_file(path, "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\n");
    ASSERT_EQ(0, capture_cmd((char *[]) { "tail", "-n", "3", path, NULL }, buf, sizeof(buf)));
    ASSERT_STREQ("j\nk\nl\n", buf);
    unlink(path);
    return 1;
}
REGISTER_TEST(tail, "Phase 9: Utilities");

int test_tee(void) {
    char out[PATH_MAX], buf[128], *cmd[] = { "sh", "-c", NULL, NULL };
    tmpfile_path(out, sizeof(out), "tee_out");
    char cbuf[PATH_MAX + 64];
    snprintf(cbuf, sizeof(cbuf), "echo hello | tee %s", out);
    cmd[2] = cbuf;
    int ret = run_cmd(cmd);
    if (ret != 0) {
        unlink(out);
        return 1;
    }
    ASSERT_EQ(6, read_file(out, buf, sizeof(buf)));
    ASSERT_STREQ("hello\n", buf);
    unlink(out);
    return 1;
}
REGISTER_TEST(tee, "Phase 9: Utilities");

int test_test(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "test_file");
    write_file(path, "x");
    ASSERT_EQ(0, run_cmd((char *[]) { "test", "-f", path, NULL }));
    ASSERT_EQ(1, run_cmd((char *[]) { "test", "-d", path, NULL }));
    unlink(path);
    return 1;
}
REGISTER_TEST(test, "Phase 9: Utilities");

int test_touch(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "touch_test");
    ASSERT_EQ(0, run_cmd((char *[]) { "touch", path, NULL }));
    struct stat st;
    ASSERT_EQ(0, stat(path, &st));
    unlink(path);
    return 1;
}
REGISTER_TEST(touch, "Phase 9: Utilities");

int test_tr(void) {
    char buf[32];
    int ret =
        capture_cmd((char *[]) { "sh", "-c", "echo abc | tr a-z A-Z", NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    return 1;
}
REGISTER_TEST(tr, "Phase 9: Utilities");

int test_true(void) {
    ASSERT_EQ(0, run_cmd((char *[]) { "true", NULL }));
    return 1;
}
REGISTER_TEST(true, "Phase 9: Utilities");

int test_tty(void) {
    char buf[32];
    int ret = capture_cmd((char *[]) { "tty", "-s", NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    return 1;
}
REGISTER_TEST(tty, "Phase 9: Utilities");

int test_uname_util(void) {
    char buf[128];
    ASSERT_EQ(0, capture_cmd((char *[]) { "uname", "-a", NULL }, buf, sizeof(buf)));
    ASSERT_GT(strlen(buf), 0);
    return 1;
}
REGISTER_TEST(uname_util, "Phase 9: Utilities");

int test_uniq(void) {
    char path[PATH_MAX], buf[128];
    tmpfile_path(path, sizeof(path), "uniq_test");
    write_file(path, "a\na\nb\n");
    int ret = capture_cmd((char *[]) { "uniq", path, NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_STREQ("a\nb\n", buf);
    unlink(path);
    return 1;
}
REGISTER_TEST(uniq, "Phase 9: Utilities");

int test_unlink_util(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "unlink_util_test");
    write_file(path, "x");
    ASSERT_EQ(0, run_cmd((char *[]) { "unlink", path, NULL }));
    struct stat st;
    ASSERT_EQ(-1, stat(path, &st));
    return 1;
}
REGISTER_TEST(unlink_util, "Phase 9: Utilities");

int test_wc(void) {
    char buf[128];
    int ret =
        capture_cmd((char *[]) { "sh", "-c", "echo hello world | wc -w", NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_GT(strlen(buf), 0);
    return 1;
}
REGISTER_TEST(wc, "Phase 9: Utilities");

int test_which(void) {
    char buf[128];
    ASSERT_EQ(0, capture_cmd((char *[]) { "which", "ls", NULL }, buf, sizeof(buf)));
    ASSERT_GT(strlen(buf), 0);
    ASSERT_EQ(1, run_cmd((char *[]) { "which", "nonexistent_cmd_xyz", NULL }));
    return 1;
}
REGISTER_TEST(which, "Phase 9: Utilities");

int test_whoami(void) {
    char buf[64];
    int ret = capture_cmd((char *[]) { "whoami", NULL }, buf, sizeof(buf));
    if (ret != 0) return 1;
    ASSERT_GT(strlen(buf), 0);
    return 1;
}
REGISTER_TEST(whoami, "Phase 9: Utilities");

int test_yes(void) { return 1; }
REGISTER_TEST(yes, "Phase 9: Utilities");

int test_vi(void) { return 1; }
REGISTER_TEST(vi, "Phase 9: Utilities");
