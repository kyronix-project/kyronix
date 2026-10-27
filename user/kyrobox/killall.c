#include "common.h"

static int sig_from_name(const char *s) {
    static const struct {
        const char *name;
        int sig;
    } tbl[] = {
        { "SIGTERM", SIGTERM }, { "TERM", SIGTERM }, { "SIGKILL", SIGKILL }, { "KILL", SIGKILL },
        { "SIGINT", SIGINT },   { "INT", SIGINT },   { "SIGHUP", SIGHUP },   { "HUP", SIGHUP },
        { "SIGQUIT", SIGQUIT }, { "QUIT", SIGQUIT }, { "SIGSTOP", SIGSTOP }, { "STOP", SIGSTOP },
        { "SIGTSTP", SIGTSTP }, { "TSTP", SIGTSTP }, { "SIGCONT", SIGCONT }, { "CONT", SIGCONT },
        { "SIGALRM", SIGALRM }, { "ALRM", SIGALRM }, { "SIGABRT", SIGABRT }, { "ABRT", SIGABRT },
        { "SIGPIPE", SIGPIPE }, { "PIPE", SIGPIPE }, { "SIGUSR1", SIGUSR1 }, { "USR1", SIGUSR1 },
        { "SIGUSR2", SIGUSR2 }, { "USR2", SIGUSR2 },
    };
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
        if (strcmp(s, tbl[i].name) == 0) return tbl[i].sig;
    return -1;
}

int main(int argc, char **argv) {
    kx_prog = "killall";

    int sig = SIGTERM;
    int first = 1;

    if (argc > 1 && argv[1][0] == '-') {
        const char *s = argv[1] + 1;
        if (s[0] && s[0] >= '0' && s[0] <= '9') {
            sig = atoi(s);
        } else {
            sig = sig_from_name(s);
            if (sig < 0) {
                fprintf(stderr, "%s: unknown signal %s\n", kx_prog, argv[1]);
                return 1;
            }
        }
        first = 2;
    }

    if (first >= argc) {
        fprintf(stderr, "usage: %s [-SIGNAL] name...\n", kx_prog);
        return 1;
    }

    FILE *f = fopen("/proc/pids", "r");
    if (!f) {
        kx_warn("/proc/pids");
        return 2;
    }

    int rc = 1;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        unsigned int pid, ppid, uid;
        char state;
        char exe[512] = { 0 };
        if (sscanf(line, "%u %u %c %u %511s", &pid, &ppid, &state, &uid, exe) < 4) continue;

        const char *name = strrchr(exe, '/');
        name = name ? name + 1 : exe;

        for (int i = first; i < argc; i++) {
            if (strcmp(name, argv[i]) == 0 && pid != (unsigned int) getpid()) {
                if (kill((pid_t) pid, sig) == 0)
                    rc = 0;
                else
                    kx_warn(argv[i]);
            }
        }
    }

    fclose(f);
    return rc;
}
