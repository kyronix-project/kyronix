#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RC_CONF "/etc/rc.conf"
#define SERVICES_STATE "/var/run/services"
#define LOG_PATH "/var/log/messages"
#define SERVICE_CTL_DIR "/var/run/servctl"
#define INIT_PID_PATH "/var/run/kyronix-init.pid"

static int valid_service_name(const char *name) {
    if (!name || !*name) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
            return 0;
    return 1;
}

static int down_path(const char *name, char *path, size_t size) {
    if (!valid_service_name(name)) {
        fprintf(stderr, "servctl: invalid service name: %s\n", name ? name : "");
        return -1;
    }
    snprintf(path, size, "%s/%s.down", SERVICE_CTL_DIR, name);
    return 0;
}

static int set_down(const char *name, int down) {
    char path[PATH_MAX];
    if (down_path(name, path, sizeof(path)) < 0) return -1;
    if (!down) {
        if (unlink(path) < 0 && errno != ENOENT) {
            fprintf(stderr, "servctl: %s: %s\n", path, strerror(errno));
            return -1;
        }
        return 0;
    }

    if (mkdir(SERVICE_CTL_DIR, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "servctl: %s: %s\n", SERVICE_CTL_DIR, strerror(errno));
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "servctl: %s: %s\n", path, strerror(errno));
        return -1;
    }
    fsync(fd);
    close(fd);
    return 0;
}

static int is_down(const char *name) {
    char path[PATH_MAX];
    return down_path(name, path, sizeof(path)) == 0 && access(path, F_OK) == 0;
}

static void usage(void) {
    fprintf(stderr, "usage: servctl <command> [name] [args...]\n");
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
        fprintf(stderr, "servctl: %s already enabled\n", name);
        return 0;
    }

    FILE *f = fopen(RC_CONF, "a");
    if (!f) { fprintf(stderr, "servctl: %s: %s\n", RC_CONF, strerror(errno)); return -1; }

    fprintf(f, "%s:", name);
    for (int i = 0; i < nargs; i++)
        fprintf(f, "%s%s", i ? " " : "", args[i]);
    fprintf(f, "\n");
    fclose(f);

    fprintf(stderr, "servctl: %s enabled\n", name);
    return 0;
}

static int rc_disable(const char *name) {
    if (!rc_find_name(name)) {
        fprintf(stderr, "servctl: %s not enabled\n", name);
        return -1;
    }

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", RC_CONF);

    FILE *in = fopen(RC_CONF, "r");
    FILE *out = fopen(tmp, "w");
    if (!in || !out) {
        fprintf(stderr, "servctl: %s: %s\n", "rewriting rc.conf", strerror(errno));
        if (in) fclose(in);
        if (out) fclose(out);
        return -1;
    }

    char line[256], parsed[256];
    int in_services = 0;

    while (fgets(line, sizeof(line), in)) {
        memcpy(parsed, line, sizeof(parsed));
        parsed[sizeof(parsed) - 1] = '\0';
        char *p = parsed + strspn(parsed, " \t");
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
    fflush(out);
    fsync(fileno(out));
    fclose(out);
    if (rename(tmp, RC_CONF) < 0) {
        fprintf(stderr, "servctl: %s: %s\n", "replacing rc.conf", strerror(errno));
        unlink(tmp);
        return -1;
    }

    fprintf(stderr, "servctl: %s disabled\n", name);
    return 0;
}

static int servctl_reload(void) {
    FILE *f = fopen(INIT_PID_PATH, "r");
    long pid = -1;
    if (f) {
        if (fscanf(f, "%ld", &pid) != 1) pid = -1;
        fclose(f);
    }
    if (pid <= 0 || pid > INT_MAX) {
        fprintf(stderr, "servctl: invalid or missing %s\n", INIT_PID_PATH);
        return -1;
    }
    if (kill((pid_t)pid, SIGHUP) < 0) {
        fprintf(stderr, "servctl: kill(%ld, SIGHUP): %s\n", pid, strerror(errno));
        return -1;
    }
    return 0;
}

static int servctl_stop(const char *name) {
    if (set_down(name, 1) < 0) return -1;
    FILE *f = fopen(SERVICES_STATE, "r");
    if (!f) {
        fprintf(stderr, "servctl: no services running\n");
        return 0;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char n[64];
        int pid;
        char state[16];
        if (sscanf(line, "%63s %d %15s", n, &pid, state) == 3) {
            if (strcmp(n, name) == 0 && pid > 0) {
                if (kill(pid, SIGTERM) < 0 && errno != ESRCH)
                    fprintf(stderr, "servctl: %s: %s\n", name, strerror(errno));
                else
                    fprintf(stderr, "servctl: %s stopped\n", name);
                fclose(f);
                return 0;
            }
        }
    }
    fclose(f);
    fprintf(stderr, "servctl: %s not running\n", name);
    return 0;
}

static void servctl_status(const char *name) {
    FILE *f = fopen(SERVICES_STATE, "r");
    if (!f) {
        fprintf(stderr, "servctl: no services running\n");
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
            printf("%-20s %s\n", n, !is_down(n) && pid > 0 ? "running" : "stopped");
        }
    }
    fclose(f);
    if (name && !found)
        printf("%-20s unknown\n", name);
}

static void servctl_logs(const char *name) {
    FILE *f = fopen(LOG_PATH, "r");
    if (!f) {
        fprintf(stderr, "servctl: no log at %s\n", LOG_PATH);
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
        servctl_reload();
        return 0;
    }

    if (strcmp(cmd, "enable") == 0) {
        if (argc < 3) { usage(); return 1; }
        const char *name = argv[2];
        if (argc < 4) {
            fprintf(stderr, "servctl: missing command for %s\n", name);
            return 1;
        }
        if (rc_enable(name, argv + 3, argc - 3) < 0) return 1;
        if (set_down(name, 0) < 0) return 1;
        if (servctl_reload() < 0)
            fprintf(stderr, "servctl: service will start on next boot (reload failed)\n");
        return 0;
    }

    if (strcmp(cmd, "disable") == 0) {
        if (argc < 3) { usage(); return 1; }
        const char *name = argv[2];
        if (rc_disable(name) < 0) return 1;
        if (servctl_reload() < 0) {
            servctl_stop(name);
        } else {
            set_down(name, 0);
        }
        return 0;
    }

    if (strcmp(cmd, "start") == 0) {
        if (argc < 3) { usage(); return 1; }
        if (!rc_find_name(argv[2])) {
            fprintf(stderr, "servctl: %s not enabled (use 'servctl enable %s <cmd>' first)\n", argv[2], argv[2]);
            return 1;
        }
        if (set_down(argv[2], 0) < 0) return 1;
        servctl_reload();
        return 0;
    }

    if (strcmp(cmd, "stop") == 0) {
        if (argc < 3) { usage(); return 1; }
        return servctl_stop(argv[2]) < 0 ? 1 : 0;
    }

    if (strcmp(cmd, "restart") == 0) {
        if (argc < 3) { usage(); return 1; }
        if (servctl_stop(argv[2]) < 0) return 1;
        sleep(1);
        if (set_down(argv[2], 0) < 0) return 1;
        if (servctl_reload() < 0) {
            fprintf(stderr, "servctl: restart %s failed\n", argv[2]);
            return 1;
        }
        return 0;
    }

    if (strcmp(cmd, "status") == 0) {
        servctl_status(argc > 2 ? argv[2] : NULL);
        return 0;
    }

    if (strcmp(cmd, "logs") == 0) {
        servctl_logs(argc > 2 ? argv[2] : NULL);
        return 0;
    }

    usage();
    return 1;
}
