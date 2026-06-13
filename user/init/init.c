#define _POSIX_C_SOURCE 200809
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STATUS_COL 72

#define COL_GRN "\033[0;32m"
#define COL_RED "\033[0;31m"
#define COL_RST "\033[0m"

#define MAX_LINE 256
#define MAX_SERVICES 32
#define MAX_ARGS 8

struct service {
    char name[64];
    char* argv[MAX_ARGS];
};

static struct service services[MAX_SERVICES];
static int nservices;

static char* trim(char* s)
{
    while (*s && isspace(*s))
        s++;
    char* e = s + strlen(s);
    while (e > s && isspace(*(e - 1)))
        e--;
    *e = '\0';
    return s;
}

static void status(const char* msg, int ok)
{
    fprintf(stderr, COL_GRN " *" COL_RST " %s ...\033[%dG[ %s%s" COL_RST " ]\n", msg, STATUS_COL,
            ok ? COL_GRN : COL_RED, ok ? "ok" : "!!");
}

static void info(const char* msg)
{
    fprintf(stderr, "%s\n", msg);
}

static void spawn(struct service* svc)
{
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        setsid();
        execvp(svc->argv[0], svc->argv);
        status(svc->name, 0);
        _exit(127);
    }
}

static void read_rc_conf(void)
{
    FILE* f = fopen("/etc/rc.conf", "r");
    if (!f) {
        status("Reading /etc/rc.conf", 0);
        return;
    }

    char line[MAX_LINE];
    int in_services = 0;

    while (fgets(line, sizeof(line), f)) {
        char* p = trim(line);
        if (!*p || *p == '#' || *p == ';')
            continue;
        if (*p == '[') {
            char* end = strchr(p + 1, ']');
            if (!end)
                continue;
            *end = '\0';
            in_services = (strcmp(p + 1, "services") == 0);
            continue;
        }
        if (!in_services || nservices >= MAX_SERVICES)
            continue;

        char* colon = strchr(p, ':');
        if (!colon)
            continue;
        *colon++ = '\0';

        char* name = trim(p);
        char* cmd = trim(colon);
        if (!*name || !*cmd)
            continue;

        strncpy(services[nservices].name, name, sizeof(services[nservices].name) - 1);

        char* token = strtok(cmd, " ");
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

int main(void)
{
    int fd = open("/dev/tty", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }

    signal(SIGCHLD, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGALRM, SIG_IGN);

    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    setenv("HOME", "/", 1);
    setenv("SHELL", "/bin/ksh", 1);
    setenv("TERM", "vt100", 1);

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

    for (;;)
        pause();
}
