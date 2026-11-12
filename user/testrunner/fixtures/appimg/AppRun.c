/* Realistic glibc AppRun for Stage G3 end-to-end sandbox testing.
 * Compiled dynamically against Debian bookworm libc, shipped inside the
 * AppImage with an $ORIGIN/libs RUNPATH so it exercises the bundled-lib +
 * loader-from-image resolution path rather than relying on a static binary. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
    /* stdout is a pipe in the test harness: unbuffered so that any late crash
     * still leaves the markers we already printed visible. */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("REAL_APPRUN ok\n");
    const char *ad = getenv("APPDIR");
    printf("APPDIR=%s\n", ad ? ad : "(null)");
    const char *ai = getenv("APPIMAGE");
    printf("APPIMAGE=%s\n", ai ? ai : "(null)");
    if (argc > 1) printf("ARG1=%s\n", argv[1]);

    FILE *m = fopen("/proc/self/maps", "r");
    if (!m) {
        perror("maps");
        printf("maps-fail\n");
    } else {
        char line[512];
        int lines = 0, exec = 0, stack = 0, bad = 0, named_exe = 0;
        unsigned long prev_end = 0;
        while (fgets(line, sizeof(line), m)) {
            unsigned long s, e, off, ino;
            unsigned ma, mi;
            char perms[5];
            if (sscanf(line, "%lx-%lx %4s %lx %x:%x %lu", &s, &e, perms, &off, &ma, &mi, &ino) != 7) {
                bad++;
                continue;
            }
            if (e <= s || (s & 0xfff) || (e & 0xfff)) bad++;
            if (s < prev_end) bad++;
            prev_end = e;
            if (perms[0] == 'r' && perms[1] == '-' && perms[2] == 'x') exec++;
            if (strstr(line, "[stack]")) stack++;
            if (strstr(line, "AppRun")) named_exe++;
            if (strstr(line, "xxxx") || strstr(line, "yyyy")) bad++;
            lines++;
        }
        fclose(m);
        printf("maps-ok lines=%d bad=%d exec=%d stack=%d named_exe=%d\n", lines, bad, exec, stack,
               named_exe);
    }

    printf("host_sentinel=%d\n", access("/etc/kyronix-host-sentinel", F_OK) == 0);
    printf("sysroot_libc=%d\n", access("/lib/x86_64-linux-gnu/libc.so.6", F_OK) == 0);
    printf("bundled_libc=%d\n", access("/app/libs/libc.so.6", F_OK) == 0);

    errno = 0;
    int tty = open("/dev/tty", O_RDWR);
    int tty_err = errno;
    printf("dev_tty_rw=%d errno=%d\n", tty >= 0, tty_err);
    if (tty >= 0) close(tty);

    errno = 0;
    int tf = open("/tmp/.kx-probe", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int tmp_err = errno;
    printf("tmp_writable=%d errno=%d\n", tf >= 0, tmp_err);
    if (tf >= 0) {
        close(tf);
        unlink("/tmp/.kx-probe");
    }

    printf("terminfo_xterm_color=%d\n", access("/etc/terminfo/x/xterm-color", F_OK) == 0);
    printf("terminfo_linux=%d\n", access("/etc/terminfo/l/linux", F_OK) == 0);

    const char *home = getenv("HOME");
    printf("home=%s home_isdir=%d\n", home ? home : "(null)",
           home && access(home, F_OK) == 0);

    char cwd[512];
    printf("cwd=%s\n", getcwd(cwd, sizeof(cwd)) ? cwd : "(fail)");
    printf("rel_file=%d\n", access("AppRun", R_OK) == 0);
    printf("rel_arg_exists=%d\n", argc > 1 && access(argv[1], R_OK) == 0);

    const char *xdg = getenv("XDG_RUNTIME_DIR");
    const char *wl = getenv("WAYLAND_DISPLAY");
    printf("xdg_runtime=%s\n", xdg ? xdg : "(null)");
    printf("wayland_display=%s\n", wl ? wl : "(null)");

    struct stat dstat;
    int xdg_dir = xdg && stat(xdg, &dstat) == 0 && S_ISDIR(dstat.st_mode);
    printf("xdg_runtime_isdir=%d\n", xdg_dir);
    printf("xdg_runtime_writable=%d\n", xdg_dir && access(xdg, W_OK) == 0);

    char sentinel[256];
    int sent_ok = 0;
    if (xdg_dir && snprintf(sentinel, sizeof(sentinel), "%s/.kx-rt-sentinel", xdg) <
                       (int) sizeof(sentinel))
        sent_ok = access(sentinel, F_OK) == 0;
    printf("xdg_sentinel=%d\n", sent_ok);

    char sockpath[256];
    int have_path = 0;
    if (wl && wl[0] == '/') {
        have_path = snprintf(sockpath, sizeof(sockpath), "%s", wl) < (int) sizeof(sockpath);
    } else if (xdg && xdg[0] && wl && wl[0]) {
        have_path = snprintf(sockpath, sizeof(sockpath), "%s/%s", xdg, wl) < (int) sizeof(sockpath);
    }
    if (!have_path) {
        printf("wl_sock_visible=0 wl_sock_issock=0 wl_sock_connect=-1\n");
    } else {
        struct stat sst;
        int visible = stat(sockpath, &sst) == 0;
        int issock = visible && S_ISSOCK(sst.st_mode);
        int conn = -1, conn_err = 0;
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_un sun;
            memset(&sun, 0, sizeof(sun));
            sun.sun_family = AF_UNIX;
            snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sockpath);
            errno = 0;
            conn = connect(fd, (struct sockaddr *) &sun, sizeof(sun));
            conn_err = errno;
            close(fd);
        }
        printf("wl_sock_visible=%d wl_sock_issock=%d wl_sock_connect=%d errno=%d\n", visible, issock,
               conn, conn_err);
    }

    printf("dev_shm_writable=%d\n", access("/dev/shm", W_OK) == 0);

    const char *ec = getenv("AI_EXIT");
    return ec ? atoi(ec) : 0;
}