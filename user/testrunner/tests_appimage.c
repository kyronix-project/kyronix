#include "test_harness.h"

#include <sys/ioctl.h>

#ifndef LOOP_GET_STATUS64
#define LOOP_GET_STATUS64 0x4C05
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

#define APPIMG "/app.img"

static int appimg_ok(void) {
    struct stat st;
    return stat(APPIMG, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 4096;
}

static int loop_free(int n) {
    char dev[32];
    snprintf(dev, sizeof(dev), "/dev/loop%d", n);
    int fd = open(dev, O_RDONLY);
    if (fd < 0) return 0;
    loop_info64_t li;
    errno = 0;
    int r = ioctl(fd, LOOP_GET_STATUS64, &li);
    int err = errno;
    close(fd);
    return r == -1 && err == ENXIO;
}

static int tmp_has_aimnt(void) {
    DIR *d = opendir("/tmp");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "aimnt-", 6) == 0) found = 1;
    }
    closedir(d);
    return found;
}

int test_appimage_basic(void) {
    if (!appimg_ok()) return 1;
    char buf[512];
    char *argv[] = { "/bin/kair", APPIMG, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "AppRun executed\n"));
    ASSERT_NOTNULL(strstr(buf, "APPDIR=/tmp/aimnt-"));
    ASSERT_NOTNULL(strstr(buf, "APPIMAGE=/app.img\n"));
    return 1;
}
REGISTER_TEST(appimage_basic, "Phase 7: AppImage");

int test_appimage_args(void) {
    if (!appimg_ok()) return 1;
    char buf[512];
    char *argv[] = { "/bin/kair", APPIMG, "hello-arg", NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "ARG1=hello-arg\n"));
    return 1;
}
REGISTER_TEST(appimage_args, "Phase 7: AppImage");

int test_appimage_exit_code(void) {
    if (!appimg_ok()) return 1;
    char buf[512];
    setenv("AI_EXIT", "7", 1);
    char *argv[] = { "/bin/kair", APPIMG, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    unsetenv("AI_EXIT");
    ASSERT_EQ(7, rc);
    ASSERT_NOTNULL(strstr(buf, "AppRun executed\n"));
    return 1;
}
REGISTER_TEST(appimage_exit_code, "Phase 7: AppImage");

int test_appimage_from_ext2(void) {
    if (!appimg_ok()) return 1;
    struct stat root_st, mnt_st;
    if (stat("/", &root_st) != 0 || stat("/mnt", &mnt_st) != 0) return 1;
    if (root_st.st_dev == mnt_st.st_dev) return 1;
    char *cp[] = { "/bin/cp", APPIMG, "/mnt/app-e2e.img", NULL };
    ASSERT_EQ(0, run_cmd(cp));
    char buf[512];
    char *argv[] = { "/bin/kair", "/mnt/app-e2e.img", NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    int ok = (rc == 0) && strstr(buf, "AppRun executed\n") && strstr(buf, "APPIMAGE=/mnt/app-e2e.img\n");
    unlink("/mnt/app-e2e.img");
    ASSERT_TRUE(ok);
    return 1;
}
REGISTER_TEST(appimage_from_ext2, "Phase 7: AppImage");

int test_appimage_relative_path(void) {
    if (!appimg_ok()) return 1;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return 1;
    chdir("/");
    char buf[512];
    char *argv[] = { "/bin/kair", "app.img", NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    chdir(cwd);
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "AppRun executed\n"));
    ASSERT_NOTNULL(strstr(buf, "APPIMAGE=/app.img\n"));
    return 1;
}
REGISTER_TEST(appimage_relative_path, "Phase 7: AppImage");

int test_appimage_cleanup(void) {
    if (!appimg_ok()) return 1;
    int free_before = loop_free(0);
    char buf[512];
    char *argv[] = { "/bin/kair", APPIMG, NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    if (free_before) ASSERT_TRUE(loop_free(0));
    ASSERT_FALSE(tmp_has_aimnt());
    return 1;
}
REGISTER_TEST(appimage_cleanup, "Phase 7: AppImage");

int test_appimage_invalid(void) {
    char buf[128];
    char *argv1[] = { "/bin/kair", "/no/such.img", NULL };
    ASSERT_EQ(1, capture_cmd(argv1, buf, sizeof(buf)));
    char *argv2[] = { "/bin/kair", "/etc/fstab", NULL };
    ASSERT_EQ(1, capture_cmd(argv2, buf, sizeof(buf)));
    char *argv3[] = { "/bin/kair", NULL };
    ASSERT_EQ(1, capture_cmd(argv3, buf, sizeof(buf)));
    return 1;
}
REGISTER_TEST(appimage_invalid, "Phase 7: AppImage");

int test_appimage_version(void) {
    char buf[128];
    char *argv[] = { "/bin/kair", "--appimage-version", NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    ASSERT_NOTNULL(strstr(buf, "kair 1.0"));
    return 1;
}
REGISTER_TEST(appimage_version, "Phase 7: AppImage");

int test_appimage_unknown_option(void) {
    char buf[128];
    char *argv[] = { "/bin/kair", "--appimage-bogus", APPIMG, NULL };
    ASSERT_EQ(1, capture_cmd(argv, buf, sizeof(buf)));
    return 1;
}
REGISTER_TEST(appimage_unknown_option, "Phase 7: AppImage");

#define APPIMG_XZ "/app_xz.img"

static int appimg_xz_ok(void) {
    struct stat st;
    return stat(APPIMG_XZ, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 4096;
}

int test_appimage_xz_basic(void) {
    if (!appimg_xz_ok()) return 1;
    char buf[512];
    char *argv[] = { "/bin/kair", APPIMG_XZ, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "AppRun executed\n"));
    ASSERT_NOTNULL(strstr(buf, "APPDIR=/tmp/aimnt-"));
    ASSERT_NOTNULL(strstr(buf, "APPIMAGE=" APPIMG_XZ "\n"));
    return 1;
}
REGISTER_TEST(appimage_xz_basic, "Phase 7: AppImage");

int test_appimage_xz_args(void) {
    if (!appimg_xz_ok()) return 1;
    char buf[512];
    char *argv[] = { "/bin/kair", APPIMG_XZ, "hello-arg", NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "ARG1=hello-arg\n"));
    return 1;
}
REGISTER_TEST(appimage_xz_args, "Phase 7: AppImage");

int test_appimage_xz_exit_code(void) {
    if (!appimg_xz_ok()) return 1;
    char buf[512];
    setenv("AI_EXIT", "7", 1);
    char *argv[] = { "/bin/kair", APPIMG_XZ, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    unsetenv("AI_EXIT");
    ASSERT_EQ(7, rc);
    ASSERT_NOTNULL(strstr(buf, "AppRun executed\n"));
    return 1;
}
REGISTER_TEST(appimage_xz_exit_code, "Phase 7: AppImage");

int test_appimage_xz_cleanup(void) {
    if (!appimg_xz_ok()) return 1;
    int free_before = loop_free(0);
    char buf[512];
    char *argv[] = { "/bin/kair", APPIMG_XZ, NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    if (free_before) ASSERT_TRUE(loop_free(0));
    ASSERT_FALSE(tmp_has_aimnt());
    return 1;
}
REGISTER_TEST(appimage_xz_cleanup, "Phase 7: AppImage");

// real glibc AppImage + sandboxing
#define REAL_APPIMG "/real_app.img"
#define SYSROOT_PRESENT "/usr/lib/sysroots/debian.sqsh"

static int real_appimg_ok(void) {
    struct stat st;
    // Both the image and the sysroot must exist for the sandbox path to run.
    return stat(REAL_APPIMG, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 4096 &&
           access(SYSROOT_PRESENT, F_OK) == 0;
}

int test_appimage_real_basic(void) {
    if (!real_appimg_ok()) return 1;
    char buf[1024];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "REAL_APPRUN ok\n"));
    ASSERT_NOTNULL(strstr(buf, "APPDIR=/tmp/aisb-"));
    ASSERT_NOTNULL(strstr(buf, "APPIMAGE=" REAL_APPIMG "\n"));
    return 1;
}
REGISTER_TEST(appimage_real_basic, "Phase 7: AppImage");

int test_appimage_real_bundled_libs(void) {
    if (!real_appimg_ok()) return 1;
    char buf[1024];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    // Dynamic glibc binary executed via bundled loader/libs inside the image 
    ASSERT_NOTNULL(strstr(buf, "bundled_libc=1\n"));
    return 1;
}
REGISTER_TEST(appimage_real_bundled_libs, "Phase 7: AppImage");

int test_appimage_real_maps(void) {
    if (!real_appimg_ok()) return 1;
    char buf[1024];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "maps-ok\n"));
    return 1;
}
REGISTER_TEST(appimage_real_maps, "Phase 7: AppImage");

int test_appimage_real_isolation(void) {
    if (!real_appimg_ok()) return 1;
    // Plant a host-only sentinel that must NOT be visible in the sandbox. 
    FILE *s = fopen("/etc/kyronix-host-sentinel", "w");
    ASSERT_NOTNULL(s);
    fputs("secret\n", s);
    fclose(s);
    char buf[1024];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    unlink("/etc/kyronix-host-sentinel");
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "host_sentinel=0\n"));
    // sysroot libc bound at the standard absolute path must be reachable 
    ASSERT_NOTNULL(strstr(buf, "sysroot_libc=1\n"));
    return 1;
}
REGISTER_TEST(appimage_real_isolation, "Phase 7: AppImage");

int test_appimage_real_args_exit(void) {
    if (!real_appimg_ok()) return 1;
    char buf[1024];
    setenv("AI_EXIT", "9", 1);
    char *argv[] = { "/bin/kair", REAL_APPIMG, "g3arg", NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    unsetenv("AI_EXIT");
    ASSERT_EQ(9, rc);
    ASSERT_NOTNULL(strstr(buf, "ARG1=g3arg\n"));
    return 1;
}
REGISTER_TEST(appimage_real_args_exit, "Phase 7: AppImage");

int test_appimage_real_cleanup(void) {
    if (!real_appimg_ok()) return 1;
    char buf[1024];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    // No sandbox residue left behind in /tmp 
    DIR *d = opendir("/tmp");
    ASSERT_NOTNULL(d);
    struct dirent *e;
    int resid = 0;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "aisb-", 5) == 0 || strncmp(e->d_name, "aisr-", 5) == 0 ||
            strncmp(e->d_name, "aimnt-", 6) == 0)
            resid = 1;
    }
    closedir(d);
    ASSERT_FALSE(resid);
    return 1;
}
REGISTER_TEST(appimage_real_cleanup, "Phase 7: AppImage");
