/* Realistic glibc AppRun for Stage G3 end-to-end sandbox testing.
 * Compiled dynamically against Debian bookworm libc, shipped inside the
 * AppImage with an $ORIGIN/libs RUNPATH so it exercises the bundled-lib +
 * loader-from-image resolution path rather than relying on a static binary. */
#include <stdio.h>
#include <stdlib.h>
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

    /* proc must be reachable inside the sandbox */
    FILE *m = fopen("/proc/self/maps", "r");
    if (m) {
        printf("maps-ok\n");
        fclose(m);
    } else {
        perror("maps");
        printf("maps-fail\n");
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
