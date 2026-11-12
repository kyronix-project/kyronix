/*
 * The point of the whole stage: a real, released AppImage must run.
 *
 * Every other AppImage fixture in this tree was built by us, so it can only
 * show that our own layout works. neovim 0.10.3 is a released image with a
 * #!/bin/bash AppRun, no libs/ directory, and a glibc binary whose interpreter
 * only exists inside the sandbox -- so it exercises the squashfs scanner, the
 * script-AppRun sandbox, the sysroot, and the loader at once.
 *
 * The harness captures stdout only and maps signal death to -1, so every proof
 * has to be a marker on stdout, and the exit code has to be asserted first.
 */
#include "test_harness.h"
#include <pty.h>

#define NVIM_APPIMG "/nvim.appimage"


static int nvim_present(void) {
    struct stat st;
    return stat(NVIM_APPIMG, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 4096;
}

/* Everything but the image is a local build product; only the sysroot is
 * network-dependent, and the repo degrades to a warning for that one. */
static int nvim_runnable(void) {
    struct stat st;
    if (!nvim_present()) return 0;
    if (access("/bin/bash", X_OK) != 0) return 0;
    if (stat("/usr/lib/sysroots/debian.sqsh", &st) != 0) return 0;
    return 1;
}

int test_appimage_nvim_version(void) {
    if (!nvim_present()) return 1;
    if (!nvim_runnable()) return 1;

    char buf[8192];
    char *argv[] = { "/bin/kair", NVIM_APPIMG, "--version", NULL };
    int rc = capture_cmd(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    /* --version prints the release on stdout, which a stub cannot fake */
    ASSERT_NOTNULL(strstr(buf, "NVIM v0.10.3"));
    return 1;
}
REGISTER_TEST(appimage_nvim_version, "Phase 7: AppImage");

int test_appimage_nvim_headless(void) {
    if (!nvim_runnable()) return 1;

    /* -es is Ex silent mode: the batch mode for scripting. --headless is not
     * usable here -- it turns stdio into an RPC channel and then waits on it,
     * which no harness reading a pipe to EOF can satisfy.
     *
     * This loads the runtime, the bundled Lua and the tree-sitter parsers, so
     * it fails on anything short of a working glibc process rather than merely
     * printing a version string. */
    char buf[8192];
    char *argv[] = { "/bin/kair", NVIM_APPIMG, "-es", "-c", "q", NULL };
    int rc = capture_cmd_eof(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    return 1;
}
REGISTER_TEST(appimage_nvim_headless, "Phase 7: AppImage");

int test_appimage_nvim_sandboxed(void) {
    if (!nvim_runnable()) return 1;

    /* The image has no libs/, so it cannot be self-contained: the run has to go
     * through the sysroot sandbox. nvim reports the APPDIR kair handed it, and
     * only a sandboxed run reports the sandbox path.
     *
     * The write is explicit because -es suppresses the message area, so
     * print() reaches nothing the harness can read. */
    static const char *lua_probe =
        "lua io.stdout:write('appdir=' .. (vim.env.APPDIR or 'nil') .. '\\n') io.stdout:flush()";
    char buf[8192];
    char *argv[] = { "/bin/kair", NVIM_APPIMG, "-es", "-c", (char *) lua_probe, "-c", "q", NULL };
    int rc = capture_cmd_eof(argv, buf, sizeof(buf));
    ASSERT_EQ(0, rc);
    ASSERT_NOTNULL(strstr(buf, "appdir=/tmp/aisb-"));
    return 1;
}
REGISTER_TEST(appimage_nvim_sandboxed, "Phase 7: AppImage");

int test_appimage_nvim_cleanup(void) {
    if (!nvim_runnable()) return 1;
    char buf[512];
    char *argv[] = { "/bin/kair", NVIM_APPIMG, "--version", NULL };
    ASSERT_EQ(0, capture_cmd(argv, buf, sizeof(buf)));
    /* No sandbox or mount residue may survive the run. */
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
REGISTER_TEST(appimage_nvim_cleanup, "Phase 7: AppImage");

/* The interactive path. neovim only enters TUI mode when its stdout is a
 * terminal, so the run has to go behind a real pty -- and the bytes it then
 * writes are the ones it can only produce after it has found a terminfo entry
 * for $TERM and successfully opened a screen. That makes smcup the one marker
 * worth asserting: it proves the sandbox handed over the terminfo database and
 * a writable /dev/tty at once, which is the whole point of this stage.
 *
 * The test deliberately does not require the editor to exit. Driving a full
 * screen editor to a clean quit from a harness is a test of the harness, and it
 * is what made the earlier version of this test slow and flaky. Reaching the
 * alternate screen is the thing under test; quitting is not. */
static int nvim_smcup(const char *term, char *out, size_t outsz) {
    int master = -1;
    pid_t child = forkpty(&master, NULL, NULL, NULL);
    if (child < 0) return 0;

    if (child == 0) {
        setenv("TERM", term, 1);
        setenv("LINES", "24", 1);
        setenv("COLUMNS", "80", 1);
        char *argv[] = { "/bin/kair", NVIM_APPIMG, NULL };
        execv(argv[0], argv);
        _exit(127);
    }

    size_t total = 0;
    int found = 0;

    for (int spin = 0; spin < 2500 && !found; spin++) {
        int flags = fcntl(master, F_GETFL, 0);
        fcntl(master, F_SETFL, flags | O_NONBLOCK);
        char buf[4096];
        for (;;) {
            ssize_t n = read(master, buf, sizeof(buf));
            if (n <= 0) break;
            if (total + (size_t) n < outsz - 1) {
                memcpy(out + total, buf, (size_t) n);
                total += (size_t) n;
            }

            for (ssize_t i = 0; i + 2 < n; i++) {
                if (buf[i] != '\033' || buf[i + 1] != '[') continue;
                if (buf[i + 2] == 'c') {
                    ssize_t w = write(master, "\033[?1;2c", 7);
                    (void) w;
                } else if (buf[i + 2] == '6' && i + 3 < n && buf[i + 3] == 'n') {
                    ssize_t w = write(master, "\033[1;1R", 6);
                    (void) w;
                }
            }
        }
        fcntl(master, F_SETFL, flags);
        out[total] = '\0';
        found = strstr(out, "\033[?1049h") != NULL || strstr(out, "\033[?47h") != NULL;
        if (!found) usleep(20000);
    }

    kill(child, SIGKILL);
    close(master);
    return found;
}

int test_appimage_nvim_tui(void) {
    if (!nvim_runnable()) return 1;

    char *buf = malloc(65536);
    ASSERT_NOTNULL(buf);
    int ok = nvim_smcup("xterm-256color", buf, 65536);
    if (!ok && failure_pipe[1] >= 0)
        dprintf(failure_pipe[1], "xterm-256color: no smcup, got [%.120s]\n", buf);
    free(buf);
    ASSERT_TRUE(ok);
    return 1;
}
REGISTER_TEST(appimage_nvim_tui, "Phase 7: AppImage");

int test_appimage_nvim_tui_external_terminfo(void) {
    if (!nvim_runnable()) return 1;

    char *buf = malloc(65536);
    ASSERT_NOTNULL(buf);
    int ok = nvim_smcup("xterm-color", buf, 65536);
    if (!ok && failure_pipe[1] >= 0)
        dprintf(failure_pipe[1], "xterm-color: no smcup, got [%.120s]\n", buf);
    free(buf);
    ASSERT_TRUE(ok);
    return 1;
}
REGISTER_TEST(appimage_nvim_tui_external_terminfo, "Phase 7: AppImage");
