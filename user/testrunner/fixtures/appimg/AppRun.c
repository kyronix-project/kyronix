/* Realistic glibc AppRun for Stage G3 end-to-end sandbox testing.
 * Compiled dynamically against Debian bookworm libc, shipped inside the
 * AppImage with an $ORIGIN/libs RUNPATH so it exercises the bundled-lib +
 * loader-from-image resolution path rather than relying on a static binary. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    /* isolation sentinel: a host-only file that must NOT be visible in the
     * private mount namespace / chroot */
    printf("host_sentinel=%d\n", access("/etc/kyronix-host-sentinel", F_OK) == 0);

    /* sysroot reachability: libc bound at the standard absolute path */
    printf("sysroot_libc=%d\n", access("/lib/x86_64-linux-gnu/libc.so.6", F_OK) == 0);

    /* bundled lib reachability: image-local libc via $ORIGIN/libs runpath */
    printf("bundled_libc=%d\n", access("/app/libs/libc.so.6", F_OK) == 0);

    const char *ec = getenv("AI_EXIT");
    return ec ? atoi(ec) : 0;
}
