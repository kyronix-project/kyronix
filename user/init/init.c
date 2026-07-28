#define _POSIX_C_SOURCE 200809
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define STATUS_COL 72

#define COL_GRN "\033[0;32m"
#define COL_RED "\033[0;31m"
#define COL_RST "\033[0m"

#define MAX_LINE 256
#define MAX_SERVICES 32
#define MAX_ARGS 8

int sethostname(const char *name, size_t len);

struct service {
    char name[64];
    char *argv[MAX_ARGS];
    pid_t pid;
    int respawns;
};

static struct service services[MAX_SERVICES];
static int nservices;
static volatile sig_atomic_t got_signal = 0;

static char *trim(char *s) {
    while (*s && isspace(*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace(*(e - 1))) e--;
    *e = '\0';
    return s;
}

static void status(const char *msg, int ok) {
    fprintf(stderr, COL_GRN " *" COL_RST " %s ...\033[%dG[ %s%s" COL_RST " ]\n", msg, STATUS_COL,
            ok ? COL_GRN : COL_RED, ok ? "ok" : "!!");
}

static void info(const char *msg) { fprintf(stderr, "%s\n", msg); }

static void spawn(struct service *svc) {
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        setsid();
        execvp(svc->argv[0], svc->argv);
        status(svc->name, 0);
        _exit(127);
    }
    if (pid > 0) {
        svc->pid = pid;
    }
}

static void reap_children(int sig) {
    (void)sig;
    got_signal = 1;
}

static void handle_reap(void) {
    int saved_errno = errno;
    int status_code;
    pid_t pid;
    while ((pid = waitpid(-1, &status_code, WNOHANG)) > 0) {
        for (int i = 0; i < nservices; i++) {
            if (services[i].pid == pid) {
                char buf[128];
                services[i].pid = 0;
                services[i].respawns++;
                if (services[i].respawns > 5) {
                    snprintf(buf, sizeof(buf), "%s (too many restarts, giving up)", services[i].name);
                    status(buf, 0);
                    continue;
                }
                snprintf(buf, sizeof(buf), "Restarting %s", services[i].name);
                spawn(&services[i]);
                status(buf, 1);
                break;
            }
        }
    }
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
        if (!*name || !*cmd) continue;

        strncpy(services[nservices].name, name, sizeof(services[nservices].name) - 1);

        char *token = strtok(cmd, " ");
        int argc = 0;
        while (token && argc < MAX_ARGS - 1) {
            services[nservices].argv[argc++] = strdup(token);
            token = strtok(NULL, " ");
        }
        services[nservices].argv[argc] = NULL;
        nservices++;
    }

    fclose(f);
    status("Reading /etc/rc.conf", 1);
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

    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    setenv("HOME", "/", 1);
    setenv("SHELL", "/bin/ksh", 1);
    setenv("TERM", "xterm-color", 1);

    apply_hostname();
    read_rc_conf();
    fprintf(stderr, "\n");

    info("INIT: Entering runlevel: 2");

    for (int i = 0; i < nservices; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Starting %s", services[i].name);
        spawn(&services[i]);
        status(buf, 1);
    }

    fprintf(stderr, "\n");

    for (;;) {
        pause();
        if (got_signal) {
            got_signal = 0;
            handle_reap();
        }
    }
}
