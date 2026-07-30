#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RC_CONF "/etc/rc.conf"
#define SERVICES_STATE "/var/run/services"
#define LOG_PATH "/var/log/messages"

static void usage(void) {
    fprintf(stderr, "usage: sv <command> [name] [args...]\n");
    fprintf(stderr, "  enable   <name> <cmd...>   add service to rc.conf\n");
    fprintf(stderr, "  disable  <name>            remove service from rc.conf\n");
    fprintf(stderr, "  start    <name>            start service\n");
    fprintf(stderr, "  stop     <name>            stop service\n");
    fprintf(stderr, "  restart  <name>            restart service\n");
    fprintf(stderr, "  status   [name]            show service status\n");
    fprintf(stderr, "  logs     [name]            show service logs\n");
    fprintf(stderr, "  reload                     reload rc.conf\n");
}

static int rc_find_name(const char *name) {
    FILE *f = fopen(RC_CONF, "r");
    if (!f) return 0;

    char line[256];
    int in_services = 0;
    int ret = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = line + strspn(line, " \t");
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        if (!*p || *p == '#' || *p == ';') continue;
        if (*p == '[') {
            char *end = strchr(p + 1, ']');
            if (!end) continue;
            *end = 0;
            in_services = (strcmp(p + 1, "services") == 0);
            continue;
        }
        if (!in_services) continue;
        char *colon = strchr(p, ':');
        if (!colon) continue;
        *colon = 0;
        if (strcmp(p, name) == 0) { ret = 1; break; }
    }
    fclose(f);
    return ret;
}

static int rc_enable(const char *name, char *const *args, int nargs) {
    if (rc_find_name(name)) {
        fprintf(stderr, "sv: %s already enabled\n", name);
        return 0;
    }

    FILE *f = fopen(RC_CONF, "a");
    if (!f) { fprintf(stderr, "sv: %s: %s\n", RC_CONF, strerror(errno)); return -1; }

    fprintf(f, "%s:", name);
    for (int i = 0; i < nargs; i++)
        fprintf(f, "%s%s", i ? " " : "", args[i]);
    fprintf(f, "\n");
    fclose(f);

    fprintf(stderr, "sv: %s enabled\n", name);
    return 0;
}

static int rc_disable(const char *name) {
    if (!rc_find_name(name)) {
        fprintf(stderr, "sv: %s not enabled\n", name);
        return -1;
    }

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", RC_CONF);

    FILE *in = fopen(RC_CONF, "r");
    FILE *out = fopen(tmp, "w");
    if (!in || !out) {
        fprintf(stderr, "sv: %s: %s\n", "rewriting rc.conf", strerror(errno));
        if (in) fclose(in);
        if (out) fclose(out);
        return -1;
    }

    char line[256];
    int in_services = 0;

    while (fgets(line, sizeof(line), in)) {
        char *p = line + strspn(line, " \t");
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        if (!*p || *p == '#' || *p == ';') { fputs(line, out); continue; }
        if (*p == '[') {
            char *end = strchr(p + 1, ']');
            if (!end) continue;
            *end = 0;
            in_services = (strcmp(p + 1, "services") == 0);
            fputs(line, out);
            continue;
        }
        if (in_services) {
            char *colon = strchr(p, ':');
            if (colon) {
                *colon = 0;
                if (strcmp(p, name) == 0) continue;
            }
        }
        fputs(line, out);
    }
    fclose(in);
    fclose(out);
    rename(tmp, RC_CONF);

    fprintf(stderr, "sv: %s disabled\n", name);
    return 0;
}

static int sv_reload(void) {
    if (kill(1, SIGHUP) < 0) {
        fprintf(stderr, "sv: kill(1, SIGHUP): %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static void sv_stop(const char *name) {
    FILE *f = fopen(SERVICES_STATE, "r");
    if (!f) {
        fprintf(stderr, "sv: no services running\n");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char n[64];
        int pid;
        char state[16];
        if (sscanf(line, "%63s %d %15s", n, &pid, state) == 3) {
            if (strcmp(n, name) == 0 && pid > 0) {
                if (kill(pid, SIGTERM) < 0 && errno != ESRCH)
                    fprintf(stderr, "sv: %s: %s\n", name, strerror(errno));
                else
                    fprintf(stderr, "sv: %s stopped\n", name);
                fclose(f);
                return;
            }
        }
    }
    fclose(f);
    fprintf(stderr, "sv: %s not running\n", name);
}

static void sv_status(const char *name) {
    FILE *f = fopen(SERVICES_STATE, "r");
    if (!f) {
        fprintf(stderr, "sv: no services running\n");
        return;
    }
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char n[64];
        int pid;
        char state[16];
        if (sscanf(line, "%63s %d %15s", n, &pid, state) == 3) {
            if (name && strcmp(n, name) != 0) continue;
            found = 1;
            printf("%-20s %s\n", n, pid > 0 ? "running" : "stopped");
        }
    }
    fclose(f);
    if (name && !found)
        printf("%-20s unknown\n", name);
}

static void sv_logs(const char *name) {
    FILE *f = fopen(LOG_PATH, "r");
    if (!f) {
        fprintf(stderr, "sv: no log at %s\n", LOG_PATH);
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (!name || strstr(line, name))
            fputs(line, stdout);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "reload") == 0) {
        sv_reload();
        return 0;
    }

    if (strcmp(cmd, "enable") == 0) {
        if (argc < 3) { usage(); return 1; }
        const char *name = argv[2];
        if (argc < 4) {
            fprintf(stderr, "sv: missing command for %s\n", name);
            return 1;
        }
        if (rc_enable(name, argv + 3, argc - 3) < 0) return 1;
        if (sv_reload() < 0)
            fprintf(stderr, "sv: service will start on next boot (reload failed)\n");
        return 0;
    }

    if (strcmp(cmd, "disable") == 0) {
        if (argc < 3) { usage(); return 1; }
        const char *name = argv[2];
        if (rc_disable(name) < 0) return 1;
        if (sv_reload() < 0)
            sv_stop(name);
        return 0;
    }

    if (strcmp(cmd, "start") == 0) {
        if (argc < 3) { usage(); return 1; }
        if (!rc_find_name(argv[2])) {
            fprintf(stderr, "sv: %s not enabled (use 'sv enable %s <cmd>' first)\n", argv[2], argv[2]);
            return 1;
        }
        sv_reload();
        return 0;
    }

    if (strcmp(cmd, "stop") == 0) {
        if (argc < 3) { usage(); return 1; }
        sv_stop(argv[2]);
        return 0;
    }

    if (strcmp(cmd, "restart") == 0) {
        if (argc < 3) { usage(); return 1; }
        sv_stop(argv[2]);
        sleep(1);
        if (sv_reload() < 0) {
            fprintf(stderr, "sv: restart %s failed\n", argv[2]);
            return 1;
        }
        return 0;
    }

    if (strcmp(cmd, "status") == 0) {
        sv_status(argc > 2 ? argv[2] : NULL);
        return 0;
    }

    if (strcmp(cmd, "logs") == 0) {
        sv_logs(argc > 2 ? argv[2] : NULL);
        return 0;
    }

    usage();
    return 1;
}
