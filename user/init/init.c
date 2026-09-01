#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STATUS_COL 72

#define COL_GRN "\033[0;32m"
#define COL_RED "\033[0;31m"
#define COL_RST "\033[0m"

#define MAX_LINE 256
#define MAX_SERVICES 32
#define MAX_ARGS 8
#define SERVICE_CTL_DIR "/var/run/servctl"
#define INIT_PID_PATH "/var/run/kyronix-init.pid"

int sethostname(const char *name, size_t len);

struct service {
    char name[64];
    char *argv[MAX_ARGS];
    pid_t pid;
    int respawns;
    int start_fd;
    time_t started;
};

static struct service services[MAX_SERVICES];
static int nservices;
static volatile sig_atomic_t got_signal = 0;
static volatile sig_atomic_t shutdown_cmd = 0;
static volatile sig_atomic_t reload_flag = 0;

static void write_services(void);

#define SERVICE_LOG_DIR "/var/log"
#define SERVICE_LOG_SUFFIX ".log"
#define QUIET_ARG "--quiet"

static bool valid_service_name(const char *name);

/* Services launched with --quiet are daemons whose output should be captured
   into /var/log/<name>.log; interactive services (e.g. login) keep the tty. */
static bool service_is_quiet(const struct service *svc) {
    if (!svc) return false;
    for (int i = 0; i < MAX_ARGS && svc->argv[i]; i++)
        if (strcmp(svc->argv[i], QUIET_ARG) == 0) return true;
    return false;
}

static void redirect_service_output(const struct service *svc) {
    if (!svc || !valid_service_name(svc->name)) return;
    char path[sizeof(SERVICE_LOG_DIR) + sizeof(svc->name) + sizeof(SERVICE_LOG_SUFFIX)];
    snprintf(path, sizeof(path), "%s/%s%s", SERVICE_LOG_DIR, svc->name, SERVICE_LOG_SUFFIX);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    if (dup2(fd, STDOUT_FILENO) < 0) { close(fd); return; }
    if (dup2(fd, STDERR_FILENO) < 0) { close(fd); return; }
    close(fd);
}

static char *trim(char *s) {
    while (*s && isspace(*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace(*(e - 1))) e--;
    *e = '\0';
    return s;
}

static bool valid_service_name(const char *name) {
    if (!name || !*name) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.') return false;
    return true;
}

static bool service_is_down(const struct service *svc) {
    if (!svc || !valid_service_name(svc->name)) return false;
    char path[sizeof(SERVICE_CTL_DIR) + sizeof(svc->name) + 8];
    snprintf(path, sizeof(path), "%s/%s.down", SERVICE_CTL_DIR, svc->name);
    return access(path, F_OK) == 0;
}

#define LOG_PATH "/var/log/messages"

static void log_event(const char *msg) {
    FILE *log = fopen(LOG_PATH, "a");
    if (!log) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[20];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(log, "%s [init] %s\n", ts, msg);
    fclose(log);
}

static void status(const char *msg, int ok) {
    fprintf(stderr, COL_GRN " *" COL_RST " %s ...\033[%dG[ %s%s" COL_RST " ]\n", msg, STATUS_COL,
            ok ? COL_GRN : COL_RED, ok ? "ok" : "!!");
    char buf[256];
    snprintf(buf, sizeof(buf), "%s ... [%s]", msg, ok ? "ok" : "!!");
    log_event(buf);
}

static void info(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    log_event(msg);
}

static int spawn_held(struct service *svc) {
    int gate[2];
    if (pipe(gate) < 0) return -1;

    pid_t pid = fork();
    if (pid == 0) {
        close(gate[1]);
        for (int i = 0; i < nservices; i++) {
            if (services[i].start_fd >= 0) close(services[i].start_fd);
        }
        char token;
        while (read(gate[0], &token, 1) < 0 && errno == EINTR) {}
        close(gate[0]);

        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        setsid();
        if (service_is_quiet(svc)) redirect_service_output(svc);
        execvp(svc->argv[0], svc->argv);
        status(svc->name, 0);
        _exit(127);
    }
    close(gate[0]);
    if (pid > 0) {
        svc->pid = pid;
        svc->start_fd = gate[1];
        svc->started = time(NULL);
        return 0;
    }
    close(gate[1]);
    return -1;
}

static void release_service(struct service *svc) {
    if (svc->start_fd < 0) return;
    close(svc->start_fd);
    svc->start_fd = -1;
}

static void reap_children(int sig) {
    (void)sig;
    got_signal = 1;
}

static void handle_reload(int sig) {
    (void)sig;
    reload_flag = 1;
}

static void handle_shutdown(int sig) {
    switch (sig) {
    case SIGUSR1: shutdown_cmd = RB_AUTOBOOT; break;
    case SIGUSR2: shutdown_cmd = RB_POWER_OFF; break;
    case SIGTERM: shutdown_cmd = RB_HALT_SYSTEM; break;
    }
}

static void do_shutdown(int cmd) {
    const char *action = (unsigned)cmd == RB_POWER_OFF ? "Powering off" :
                         (unsigned)cmd == RB_HALT_SYSTEM ? "Halting" : "Rebooting";
    info(action);
    status("Shutting down services", 1);
    for (int i = 0; i < nservices; i++) {
        if (services[i].pid > 0)
            kill(services[i].pid, SIGTERM);
    }

    sync();
    sleep(1);

    for (int i = 0; i < nservices; i++) {
        if (services[i].pid > 0)
            kill(services[i].pid, SIGKILL);
    }

    status("Halting system", 1);
    fflush(stderr);
    log_event("system halted");
    sync();

    if ((unsigned)cmd == RB_POWER_OFF)
        reboot(RB_POWER_OFF);
    else if ((unsigned)cmd == RB_HALT_SYSTEM)
        reboot(RB_HALT_SYSTEM);
    else
        reboot(RB_AUTOBOOT);

    for (;;) pause();
}

static void handle_reap(void) {
    int saved_errno = errno;
    int status_code;
    pid_t pid;
    bool changed = false;
    while ((pid = waitpid(-1, &status_code, WNOHANG)) > 0) {
        for (int i = 0; i < nservices; i++) {
            if (services[i].pid == pid) {
                char buf[128];
                services[i].pid = 0;
                changed = true;
                if (service_is_down(&services[i])) {
                    services[i].respawns = 0;
                    snprintf(buf, sizeof(buf), "Stopped %.63s", services[i].name);
                    info(buf);
                    break;
                }
                if (services[i].started && time(NULL) - services[i].started >= 30)
                    services[i].respawns = 0;
                services[i].respawns++;
                if (services[i].respawns > 5) {
                    snprintf(buf, sizeof(buf), "%.63s (too many restarts, giving up)",
                             services[i].name);
                    status(buf, 0);
                    continue;
                }
                snprintf(buf, sizeof(buf), "Restarting %.63s", services[i].name);
                int started = spawn_held(&services[i]) == 0;
                status(buf, started);
                fflush(stderr);
                if (started) release_service(&services[i]);
                break;
            }
        }
    }
    if (changed) write_services();
    errno = saved_errno;
}

static void apply_hostname(void) {
    FILE *f = fopen("/etc/hostname", "r");
    if (!f) return;

    char line[64];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        status("Setting hostname", 0);
        return;
    }
    fclose(f);

    char *hostname = trim(line);
    if (!*hostname ||
        sethostname(hostname, strlen(hostname)) < 0) {
        status("Setting hostname", 0);
        return;
    }
    status("Setting hostname", 1);
}

static void read_rc_conf(void) {
    FILE *f = fopen("/etc/rc.conf", "r");
    if (!f) {
        status("Reading /etc/rc.conf", 0);
        return;
    }

    char line[MAX_LINE];
    int in_services = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';') continue;
        if (*p == '[') {
            char *end = strchr(p + 1, ']');
            if (!end) continue;
            *end = '\0';
            in_services = (strcmp(p + 1, "services") == 0);
            continue;
        }
        if (!in_services || nservices >= MAX_SERVICES) continue;

        char *colon = strchr(p, ':');
        if (!colon) continue;
        *colon++ = '\0';

        char *name = trim(p);
        char *cmd = trim(colon);
        if (!valid_service_name(name) || !*cmd) continue;

        strncpy(services[nservices].name, name, sizeof(services[nservices].name) - 1);

        char *token = strtok(cmd, " ");
        int argc = 0;
        while (token && argc < MAX_ARGS - 1) {
            services[nservices].argv[argc++] = strdup(token);
            token = strtok(NULL, " ");
        }
        services[nservices].argv[argc] = NULL;
        services[nservices].start_fd = -1;
        nservices++;
    }

    fclose(f);
    status("Reading /etc/rc.conf", 1);
}

static void write_services(void) {
    FILE *f = fopen("/var/run/services.tmp", "w");
    if (!f) return;
    for (int i = 0; i < nservices; i++)
        fprintf(f, "%-20s %d %s\n", services[i].name, services[i].pid,
                services[i].pid > 0 ? "running" : "stopped");
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename("/var/run/services.tmp", "/var/run/services");
}

static void write_init_pid(void) {
    FILE *f = fopen(INIT_PID_PATH ".tmp", "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)getpid());
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(INIT_PID_PATH ".tmp", INIT_PID_PATH);
}

static void reload_config(void) {
    struct service old[MAX_SERVICES];
    int old_n = nservices;
    memcpy(old, services, sizeof(old));

    nservices = 0;
    read_rc_conf();

    for (int i = 0; i < old_n; i++) {
        bool matched = false;
        for (int j = 0; j < nservices; j++) {
            if (strcmp(old[i].name, services[j].name) == 0) {
                if (old[i].pid > 0) {
                    services[j].pid = old[i].pid;
                    services[j].start_fd = old[i].start_fd;
                    services[j].respawns = old[i].respawns;
                    services[j].started = old[i].started;
                }
                matched = true;
                break;
            }
        }
        if (!matched && old[i].pid > 0) {
            kill(old[i].pid, SIGTERM);
        }
        for (int k = 0; k < MAX_ARGS && old[i].argv[k]; k++)
            free(old[i].argv[k]);
    }

    for (int i = 0; i < nservices; i++) {
        if (services[i].pid == 0 && !service_is_down(&services[i])) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Starting %.63s", services[i].name);
            status(buf, spawn_held(&services[i]) == 0);
        }
    }
    write_services();
    for (int i = 0; i < nservices; i++) release_service(&services[i]);
}

int main(void) {
    int fd = open("/dev/tty", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }

    signal(SIGINT, SIG_IGN);
    signal(SIGALRM, SIG_IGN);
    signal(SIGCHLD, reap_children);
    signal(SIGUSR1, handle_shutdown);
    signal(SIGUSR2, handle_shutdown);
    signal(SIGTERM, handle_shutdown);
    signal(SIGHUP, handle_reload);

    mkdir("/var/log", 0755);
    mkdir("/var/run", 0755);
    mkdir(SERVICE_CTL_DIR, 0755);
    write_init_pid();
    log_event("init started");

    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    setenv("HOME", "/", 1);
    setenv("SHELL", "/bin/ksh", 1);
    setenv("TERM", "xterm-color", 1);

    apply_hostname();
    read_rc_conf();
    fprintf(stderr, "\n");

    info("INIT: Entering runlevel: 2");

    for (int i = 0; i < nservices; i++) {
        if (service_is_down(&services[i])) continue;
        char buf[128];
        snprintf(buf, sizeof(buf), "Starting %.63s", services[i].name);
        status(buf, spawn_held(&services[i]) == 0);
    }

    fprintf(stderr, "\n");
    fflush(stderr);
    for (int i = 0; i < nservices; i++) release_service(&services[i]);
    write_services();

    for (;;) {
        pause();
        if (reload_flag) {
            reload_flag = 0;
            reload_config();
        }
        if (shutdown_cmd) {
            int cmd = shutdown_cmd;
            shutdown_cmd = -1;
            do_shutdown(cmd);
            shutdown_cmd = 0;
        }
        if (got_signal) {
            got_signal = 0;
            handle_reap();
        }
    }
}
