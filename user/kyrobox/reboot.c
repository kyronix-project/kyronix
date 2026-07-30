#include "common.h"

#include <sys/reboot.h>

static int sig_for_cmd(int cmd) {
    switch (cmd) {
    case RB_POWER_OFF:  return SIGUSR2;
    case RB_HALT_SYSTEM: return SIGTERM;
    default:            return SIGUSR1;
    }
}

static void usage(void) {
    fprintf(stderr, "usage: %s [-f] [-p]\n", kx_prog);
    fprintf(stderr, "  reboot   restart the machine\n");
    fprintf(stderr, "  poweroff power the machine off\n");
    fprintf(stderr, "  halt     halt the CPU\n");
    fprintf(stderr, "  -f       force (direct syscall, skip init)\n");
    fprintf(stderr, "  -p       power off (for reboot)\n");
}

int main(int argc, char **argv) {
    kx_prog = kx_base(argv[0]);

    int cmd;
    if (strcmp(kx_prog, "poweroff") == 0)
        cmd = RB_POWER_OFF;
    else if (strcmp(kx_prog, "halt") == 0)
        cmd = RB_HALT_SYSTEM;
    else
        cmd = RB_AUTOBOOT;

    bool force = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-f") == 0)
            force = true;
        else if (strcmp(a, "-p") == 0)
            cmd = RB_POWER_OFF;
        else if (strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else {
            usage();
            return 1;
        }
    }

    if (force) {
        if (reboot(cmd) < 0) {
            fprintf(stderr, "%s: %s\n", kx_prog, strerror(errno));
            return 1;
        }
        return 0;
    }

    if (kill(1, sig_for_cmd(cmd)) < 0) {
        fprintf(stderr, "%s: init not responding, rebooting directly\n", kx_prog);
        if (reboot(cmd) < 0) {
            fprintf(stderr, "%s: %s\n", kx_prog, strerror(errno));
            return 1;
        }
    }
    return 0;
}
