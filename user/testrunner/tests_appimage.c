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
    char *argv[] = { "/bin/appimage-run", APPIMG, NULL };
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
    char *argv[] = { "/bin/appimage-run", APPIMG, "hello-arg", NULL };
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
    char *argv[] = { "/bin/appimage-run", APPIMG, NULL };
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
    char *argv[] = { "/bin/appimage-run", "/mnt/app-e2e.img", NULL };
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
    char *argv[] = { "/bin/appimage-run", "app.img", NULL };
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
    char *argv[] = { "/bin/appimage-run", APPIMG, NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    if (free_before) ASSERT_TRUE(loop_free(0));
    ASSERT_FALSE(tmp_has_aimnt());
    return 1;
}
REGISTER_TEST(appimage_cleanup, "Phase 7: AppImage");

int test_appimage_invalid(void) {
    char buf[128];
    char *argv1[] = { "/bin/appimage-run", "/no/such.img", NULL };
    ASSERT_EQ(1, capture_cmd(argv1, buf, sizeof(buf)));
    char *argv2[] = { "/bin/appimage-run", "/etc/fstab", NULL };
    ASSERT_EQ(1, capture_cmd(argv2, buf, sizeof(buf)));
    char *argv3[] = { "/bin/appimage-run", NULL };
    ASSERT_EQ(1, capture_cmd(argv3, buf, sizeof(buf)));
    return 1;
}
REGISTER_TEST(appimage_invalid, "Phase 7: AppImage");

int test_appimage_version(void) {
    char buf[128];
    char *argv[] = { "/bin/appimage-run", "--appimage-version", NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    ASSERT_NOTNULL(strstr(buf, "appimage-run 1.0"));
    return 1;
}
REGISTER_TEST(appimage_version, "Phase 7: AppImage");

int test_appimage_unknown_option(void) {
    char buf[128];
    char *argv[] = { "/bin/appimage-run", "--appimage-bogus", APPIMG, NULL };
    ASSERT_EQ(1, capture_cmd(argv, buf, sizeof(buf)));
    return 1;
}
REGISTER_TEST(appimage_unknown_option, "Phase 7: AppImage");
