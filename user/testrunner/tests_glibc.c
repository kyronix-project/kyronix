#include "test_harness.h"

static int glibc_ok(void) {
    struct stat st;
    return stat("/glibc/hello-glibc", &st) == 0 && (st.st_mode & 0111);
}

int test_glibc_direct_exec(void) {
    if (!glibc_ok()) return 1;
    setenv("LD_LIBRARY_PATH", "/glibc", 1);
    char buf[256];
    char *argv[] = { "/glibc/hello-glibc", "arg-one", NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "glibc hello: argc=1 argv1=arg-one"));
    return 1;
}
REGISTER_TEST(glibc_direct_exec, "Phase 8: glibc");

int test_glibc_loader_invoked(void) {
    if (!glibc_ok()) return 1;
    char buf[256];
    char *argv[] = { "/lib64/ld-linux-x86-64.so.2", "--library-path", "/glibc",
                     "/glibc/hello-glibc", "via-loader", NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "argv1=via-loader"));
    return 1;
}
REGISTER_TEST(glibc_loader_invoked, "Phase 8: glibc");

int test_glibc_env(void) {
    if (!glibc_ok()) return 1;
    setenv("LD_LIBRARY_PATH", "/glibc", 1);
    setenv("GLIBC_MSG", "green", 1);
    char buf[256];
    char *argv[] = { "/glibc/hello-glibc", NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    unsetenv("GLIBC_MSG");
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "env msg: green"));
    return 1;
}
REGISTER_TEST(glibc_env, "Phase 8: glibc");

int test_glibc_exit_code(void) {
    if (!glibc_ok()) return 1;
    setenv("LD_LIBRARY_PATH", "/glibc", 1);
    setenv("GLIBC_EXIT", "3", 1);
    char buf[256];
    char *argv[] = { "/glibc/hello-glibc", NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    unsetenv("GLIBC_EXIT");
    ASSERT_EQ(3, rc);
    return 1;
}
REGISTER_TEST(glibc_exit_code, "Phase 8: glibc");
