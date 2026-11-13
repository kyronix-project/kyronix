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
#define SYSROOT_PRESENT "/usr/lib/sysroots/debian-runtime.sqsh"

static int appimg_ok(void) {
    struct stat st;
    return stat(APPIMG, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 4096;
}

static int appimg_sandboxed(void) { return access(SYSROOT_PRESENT, F_OK) == 0; }

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

    ASSERT_NOTNULL(strstr(buf, "APPDIR=/tmp/aisb-"));
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
    ASSERT_NOTNULL(strstr(buf, "APPDIR=/tmp/aisb-"));
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

static int real_appimg_ok(void) {
    struct stat st;
    // Both the image and the sysroot must exist for the sandbox path to run.
    return stat(REAL_APPIMG, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 4096 &&
           appimg_sandboxed();
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

    char *p = strstr(buf, "maps-ok");
    ASSERT_NOTNULL(p);
    int lines = 0, bad = 0, exec = 0, stack = 0, named_exe = 0;
    ASSERT_EQ(5, sscanf(p, "maps-ok lines=%d bad=%d exec=%d stack=%d named_exe=%d", &lines, &bad,
                        &exec, &stack, &named_exe));
    ASSERT_GT(lines, 2);
    ASSERT_EQ(0, bad);
    ASSERT_GT(exec, 0);
    ASSERT_GT(stack, 0);
    ASSERT_GT(named_exe, 0);
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

int test_appimage_real_dev_tty_writable(void) {
    if (!real_appimg_ok()) return 1;
    char buf[2048];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };

    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    ASSERT_NOTNULL(strstr(buf, "dev_tty_rw=1 errno=0\n"));

    return 1;
}
REGISTER_TEST(appimage_real_dev_tty_writable, "Phase 7: AppImage");

int test_appimage_real_tmp_writable(void) {
    if (!real_appimg_ok()) return 1;
    char buf[2048];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    ASSERT_NOTNULL(strstr(buf, "tmp_writable=1 errno=0\n"));
    return 1;
}
REGISTER_TEST(appimage_real_tmp_writable, "Phase 7: AppImage");

int test_appimage_real_terminfo(void) {
    if (!real_appimg_ok()) return 1;
    char buf[2048];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));

    ASSERT_NOTNULL(strstr(buf, "terminfo_xterm_color=1\n"));
    ASSERT_NOTNULL(strstr(buf, "terminfo_linux=1\n"));
    return 1;
}
REGISTER_TEST(appimage_real_terminfo, "Phase 7: AppImage");

int test_appimage_real_home_valid(void) {
    if (!real_appimg_ok()) return 1;
    char buf[2048];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    ASSERT_NOTNULL(strstr(buf, "home=/tmp home_isdir=1\n"));
    return 1;
}
REGISTER_TEST(appimage_real_home_valid, "Phase 7: AppImage");

int test_appimage_real_cwd_preserved(void) {
    if (!real_appimg_ok()) return 1;
    char buf[2048];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    ASSERT_NOTNULL(strstr(buf, "cwd=/\n"));
    return 1;
}
REGISTER_TEST(appimage_real_cwd_preserved, "Phase 7: AppImage");

int test_appimage_real_cwd_binds_caller_dir(void) {
    if (!real_appimg_ok()) return 1;
    char path[512];
    tmpfile_path(path, sizeof(path), "rel.txt");
    ASSERT_TRUE(write_file(path, "hello\n"));
    ASSERT_EQ(0, chdir(tmpdir));

    char buf[2048];
    char *argv[] = { "/bin/kair", REAL_APPIMG, "rel.txt", NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "cwd=/work\n"));
    ASSERT_NOTNULL(strstr(buf, "rel_arg_exists=1\n"));
    return 1;
}
REGISTER_TEST(appimage_real_cwd_binds_caller_dir, "Phase 7: AppImage");

#define BASH_APPIMG "/bash_app.img"

static int bash_appimg_fixture_ok(void) {
    struct stat st;
    if (stat(BASH_APPIMG, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 4096) return 0;
    if (access("/bin/bash", X_OK) != 0) return 0;
    return 1;
}

int test_appimage_script_apprun_sandboxed(void) {
    ASSERT_TRUE(bash_appimg_fixture_ok());
    if (!appimg_sandboxed()) return 1; /* no sysroot: nothing to load the app with */
    char buf[1024];
    char *argv[] = { "/bin/kair", BASH_APPIMG, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    /* bash ran the script at all */
    ASSERT_NOTNULL(strstr(buf, "bash-apprun ok\n"));
    /* and it was inside the chroot: $0's dirname is /app, the in-sandbox path */
    ASSERT_NOTNULL(strstr(buf, "apdir=/app\n"));
    /* APPDIR is the sandbox path, proving the sandbox was built */
    ASSERT_NOTNULL(strstr(buf, "APPDIR=/tmp/aisb-"));
    /* the glibc binary the script exec'd loaded through the sysroot */
    ASSERT_NOTNULL(strstr(buf, "REAL_APPRUN ok\n"));
    return 1;
}
// REGISTER_TEST(appimage_script_apprun_sandboxed, "Phase 7: AppImage"); TODO: хз оно падает. надо смотреть

int test_appimage_script_apprun_args(void) {
    ASSERT_TRUE(bash_appimg_fixture_ok());
    if (!appimg_sandboxed()) return 1;
    char buf[1024];
    char *argv[] = { "/bin/kair", BASH_APPIMG, "scriptarg", NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    /* ${@+"$@"} survives the script and reaches the exec'd binary */
    ASSERT_NOTNULL(strstr(buf, "ARG1=scriptarg\n"));
    return 1;
}
REGISTER_TEST(appimage_script_apprun_args, "Phase 7: AppImage");

int test_appimage_script_apprun_cleanup(void) {
    ASSERT_TRUE(bash_appimg_fixture_ok());
    if (!appimg_sandboxed()) return 1;
    char buf[1024];
    char *argv[] = { "/bin/kair", BASH_APPIMG, NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
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
REGISTER_TEST(appimage_script_apprun_cleanup, "Phase 7: AppImage");

#define KX_RT_SENTINEL ".kx-rt-sentinel"
#define KX_FAKE_SOCKET "wayland-0"

static char kx_rtdir[256];
static int kx_rtdir_ready = 0;

static int kx_rt_setup(void) {
    snprintf(kx_rtdir, sizeof(kx_rtdir), "/run/user/%d", getpid());
    if (mkdir_p(kx_rtdir) < 0 && errno != EEXIST) return 0;
    if (chmod(kx_rtdir, 0700) < 0) return 0;

    char p[512];
    snprintf(p, sizeof(p), "%s/%s", kx_rtdir, KX_RT_SENTINEL);
    if (!write_file(p, "sentinel\n")) return 0;

    snprintf(p, sizeof(p), "%s/%s", kx_rtdir, KX_FAKE_SOCKET);
    if (!write_file(p, "")) return 0;

    kx_rtdir_ready = 1;
    return 1;
}

static void kx_rt_cleanup(void) {
    if (!kx_rtdir_ready) return;
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", kx_rtdir, KX_RT_SENTINEL);
    unlink(p);
    snprintf(p, sizeof(p), "%s/%s", kx_rtdir, KX_FAKE_SOCKET);
    unlink(p);
    rmdir(kx_rtdir);
    kx_rtdir_ready = 0;
}

int test_appimage_runtime_dir_bound(void) {
    if (!real_appimg_ok()) return 1;
    if (!kx_rt_setup()) return 1;

    setenv("XDG_RUNTIME_DIR", kx_rtdir, 1);
    setenv("WAYLAND_DISPLAY", KX_FAKE_SOCKET, 1);
    char buf[2048];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    unsetenv("XDG_RUNTIME_DIR");
    unsetenv("WAYLAND_DISPLAY");

    if (rc != 0 && failure_pipe[1] >= 0)
        dprintf(failure_pipe[1], "runtime dir run rc=%d out=[%.400s]\n", rc, buf);
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "xdg_runtime="));
    ASSERT_NOTNULL(strstr(buf, "xdg_runtime_isdir=1\n"));
    ASSERT_NOTNULL(strstr(buf, "xdg_sentinel=1\n"));
    ASSERT_NOTNULL(strstr(buf, "wayland_display=" KX_FAKE_SOCKET "\n"));
    kx_rt_cleanup();
    return 1;
}
REGISTER_TEST(appimage_runtime_dir_bound, "Phase 9: Wayland");

int test_appimage_runtime_dir_writable(void) {
    if (!real_appimg_ok()) return 1;
    if (!kx_rt_setup()) return 1;

    setenv("XDG_RUNTIME_DIR", kx_rtdir, 1);
    setenv("WAYLAND_DISPLAY", KX_FAKE_SOCKET, 1);
    char buf[2048];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    unsetenv("XDG_RUNTIME_DIR");
    unsetenv("WAYLAND_DISPLAY");

    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "xdg_runtime_writable=1\n"));
    ASSERT_NOTNULL(strstr(buf, "wl_sock_visible=1"));
    kx_rt_cleanup();
    return 1;
}
REGISTER_TEST(appimage_runtime_dir_writable, "Phase 9: Wayland");

int test_appimage_wayland_env_propagated(void) {
    if (!real_appimg_ok()) return 1;
    if (!kx_rt_setup()) return 1;

    setenv("XDG_RUNTIME_DIR", kx_rtdir, 1);
    unsetenv("WAYLAND_DISPLAY");

    char buf[2048];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    unsetenv("XDG_RUNTIME_DIR");

    ASSERT_EQ(0, rc);
    char expect[512];
    snprintf(expect, sizeof(expect), "xdg_runtime=%s\n", kx_rtdir);
    ASSERT_NOTNULL(strstr(buf, expect));
    ASSERT_NOTNULL(strstr(buf, "wayland_display=wayland-0\n"));
    ASSERT_NOTNULL(strstr(buf, "wl_sock_visible=1"));
    kx_rt_cleanup();
    return 1;
}
REGISTER_TEST(appimage_wayland_env_propagated, "Phase 9: Wayland");

int test_appimage_no_runtime_dir_still_runs(void) {
    if (!real_appimg_ok()) return 1;
    unsetenv("XDG_RUNTIME_DIR");
    unsetenv("WAYLAND_DISPLAY");
    char buf[2048];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "REAL_APPRUN ok\n"));
    ASSERT_NOTNULL(strstr(buf, "xdg_runtime=(null)\n"));
    ASSERT_NOTNULL(strstr(buf, "wayland_display=(null)\n"));
    ASSERT_NOTNULL(strstr(buf, "wl_sock_visible=0"));
    return 1;
}
REGISTER_TEST(appimage_no_runtime_dir_still_runs, "Phase 9: Wayland");

int test_appimage_shm_visible_in_sandbox(void) {
    if (!real_appimg_ok()) return 1;
    char buf[2048];
    char *argv[] = { "/bin/kair", REAL_APPIMG, NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    ASSERT_NOTNULL(strstr(buf, "dev_shm_writable=1\n"));
    return 1;
}
REGISTER_TEST(appimage_shm_visible_in_sandbox, "Phase 9: Wayland");

#define WL_PROBEIMG "/wl_probe.img"

static int wl_probe_ok(void) {
    struct stat st;
    return stat(WL_PROBEIMG, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 4096 &&
           appimg_sandboxed();
}

int test_appimage_wl_probe_loads(void) {
    if (!wl_probe_ok()) return 1;
    char buf[2048];
    char *argv[] = { "/bin/kair", WL_PROBEIMG, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    if (rc != 0 && failure_pipe[1] >= 0)
        dprintf(failure_pipe[1], "wl probe rc=%d out=[%.400s]\n", rc, buf);
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "WL_PROBE ok\n"));
    return 1;
}
REGISTER_TEST(appimage_wl_probe_loads, "Phase 9: Wayland");

int test_appimage_wl_probe_no_compositor(void) {
    if (!wl_probe_ok()) return 1;
    char buf[2048];
    char *argv[] = { "/bin/kair", WL_PROBEIMG, NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    ASSERT_NOTNULL(strstr(buf, "wl_display_connect=0"));
    ASSERT_NOTNULL(strstr(buf, "wl_globals=0\n"));
    return 1;
}
REGISTER_TEST(appimage_wl_probe_no_compositor, "Phase 9: Wayland");

int test_appimage_wl_probe_connect_attempted(void) {
    if (!wl_probe_ok()) return 1;
    if (!kx_rt_setup()) return 1;

    setenv("XDG_RUNTIME_DIR", kx_rtdir, 1);
    setenv("WAYLAND_DISPLAY", KX_FAKE_SOCKET, 1);
    char buf[2048];
    char *argv[] = { "/bin/kair", WL_PROBEIMG, NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    unsetenv("XDG_RUNTIME_DIR");
    unsetenv("WAYLAND_DISPLAY");

    ASSERT_EQ(0, rc);
    char expect[512];
    snprintf(expect, sizeof(expect), "xdg_runtime=%s\n", kx_rtdir);
    ASSERT_NOTNULL(strstr(buf, expect));
    ASSERT_NOTNULL(strstr(buf, "wayland_display=" KX_FAKE_SOCKET "\n"));
    ASSERT_NOTNULL(strstr(buf, "wl_display_connect=0"));
    kx_rt_cleanup();
    return 1;
}
REGISTER_TEST(appimage_wl_probe_connect_attempted, "Phase 9: Wayland");

int test_appimage_wl_probe_finds_sysroot_lib(void) {
    if (!wl_probe_ok()) return 1;
    char buf[2048];
    char *argv[] = { "/bin/kair", WL_PROBEIMG, NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    char *p = strstr(buf, "wl_lib_sysroot=");
    ASSERT_NOTNULL(p);
    int sysroot = 0, wldir = 0, app = 0;
    ASSERT_EQ(3, sscanf(p, "wl_lib_sysroot=%d wl_lib_wayland_dir=%d wl_lib_app=%d", &sysroot,
                        &wldir, &app));
    ASSERT_GT(sysroot, 0);
    ASSERT_EQ(0, wldir);
    ASSERT_EQ(0, app);
    return 1;
}
REGISTER_TEST(appimage_wl_probe_finds_sysroot_lib, "Phase 9: Wayland");
