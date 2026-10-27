#include "common.h"
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>

typedef struct {
    unsigned pid, ppid, uid;
    char state;
    unsigned long mem_kb;
    char exe[512];
} pinfo_t;

static int delay_ms = 2000;
static int iterations = -1;
static bool batch;
static unsigned filter_pid;
static bool raw_active;
static struct termios orig_tio;
static volatile int stop_flag;

static char *read_proc(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *buf = NULL;
    size_t cap = 0, len = 0;
    char chunk[512];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (len + got + 1 > cap) {
            cap = cap ? cap * 2 : 1024;
            while (len + got + 1 > cap) cap *= 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + len, chunk, got);
        len += got;
    }
    fclose(f);
    if (!buf) buf = malloc(1);
    buf[len] = 0;
    return buf;
}

static unsigned long parse_kb_field(const char *buf, const char *key) {
    const char *p = strstr(buf, key);
    if (!p) return 0;
    p += strlen(key);
    return strtoul(p, NULL, 10);
}

static int load_procs(pinfo_t **out) {
    char *data = read_proc("/proc/pids");
    if (!data) {
        *out = NULL;
        return 0;
    }
    pinfo_t *procs = NULL;
    int n = 0, cap = 0;
    char *saveptr = NULL;
    for (char *line = strtok_r(data, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        pinfo_t p;
        memset(&p, 0, sizeof(p));
        if (sscanf(line, "%u %u %c %u %lu %511s", &p.pid, &p.ppid, &p.state, &p.uid, &p.mem_kb,
                   p.exe) < 6)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            procs = realloc(procs, (size_t) cap * sizeof(*procs));
        }
        procs[n++] = p;
    }
    free(data);
    *out = procs;
    return n;
}

static int cmp_mem_desc(const void *a, const void *b) {
    const pinfo_t *pa = a, *pb = b;
    if (pa->mem_kb != pb->mem_kb) return pa->mem_kb > pb->mem_kb ? -1 : 1;
    return pa->pid < pb->pid ? -1 : (pa->pid > pb->pid ? 1 : 0);
}

static void restore_term(void) {
    if (!raw_active) return;
    raw_active = false;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_tio);
    fputs("\033[?25h\033[0m", stdout);
}

static void set_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &orig_tio);
    struct termios raw = orig_tio;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_active = true;
    fputs("\033[?25l", stdout);
}

static void get_win_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return;
    }
    *rows = 24;
    *cols = 80;
}

static void sig_handler(int sig) {
    (void) sig;
    stop_flag = 1;
}

static void format_uptime(unsigned long secs, char *buf, size_t n) {
    unsigned long days = secs / 86400;
    unsigned long hours = (secs % 86400) / 3600;
    unsigned long mins = (secs % 3600) / 60;
    if (days > 0)
        snprintf(buf, n, "%lu day%s, %lu:%02lu", days, days == 1 ? "" : "s", hours, mins);
    else if (hours > 0)
        snprintf(buf, n, "%lu:%02lu", hours, mins);
    else
        snprintf(buf, n, "%lu min", mins);
}

static const char *lookup_user(unsigned uid, char *buf, size_t n) {
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name) {
        snprintf(buf, n, "%s", pw->pw_name);
        return buf;
    }
    snprintf(buf, n, "%u", uid);
    return buf;
}

static void draw(bool interactive) {
    int rows = 0, cols = 0;
    if (interactive) get_win_size(&rows, &cols);
    (void) cols;

    char *uptime = read_proc("/proc/uptime");
    char *loadavg = read_proc("/proc/loadavg");
    char *meminfo = read_proc("/proc/meminfo");
    unsigned long up_s = uptime ? strtoul(uptime, NULL, 10) : 0;
    unsigned long mem_total = meminfo ? parse_kb_field(meminfo, "MemTotal:") : 0;
    unsigned long mem_free = meminfo ? parse_kb_field(meminfo, "MemFree:") : 0;
    unsigned long mem_used = mem_total > mem_free ? mem_total - mem_free : 0;
    unsigned long swap_total = meminfo ? parse_kb_field(meminfo, "SwapTotal:") : 0;
    unsigned long swap_free = meminfo ? parse_kb_field(meminfo, "SwapFree:") : 0;
    unsigned long swap_used = swap_total > swap_free ? swap_total - swap_free : 0;
    double l1 = 0, l2 = 0, l3 = 0;
    if (loadavg) sscanf(loadavg, "%lf %lf %lf", &l1, &l2, &l3);

    char uptime_buf[64];
    format_uptime(up_s, uptime_buf, sizeof(uptime_buf));
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clock_buf[16];
    strftime(clock_buf, sizeof(clock_buf), "%H:%M:%S", &tm);

    pinfo_t *procs;
    int n = load_procs(&procs);
    if (filter_pid) {
        int m = 0;
        for (int i = 0; i < n; i++)
            if (procs[i].pid == filter_pid) procs[m++] = procs[i];
        n = m;
    }
    qsort(procs, (size_t) n, sizeof(*procs), cmp_mem_desc);

    int running = 0, sleeping = 0, zombie = 0;
    for (int i = 0; i < n; i++) {
        if (procs[i].state == 'R')
            running++;
        else if (procs[i].state == 'S')
            sleeping++;
        else if (procs[i].state == 'Z')
            zombie++;
    }

    if (interactive) fputs("\033[H\033[J", stdout);
    printf("top - %s up %s,  load average: %.2f, %.2f, %.2f\n", clock_buf, uptime_buf, l1, l2, l3);
    printf("Tasks: %d total, %d running, %d sleeping, %d zombie\n", n, running, sleeping, zombie);
    printf("MiB Mem : %8.1f total, %8.1f free, %8.1f used\n", mem_total / 1024.0, mem_free / 1024.0,
           mem_used / 1024.0);
    printf("MiB Swap: %8.1f total, %8.1f free, %8.1f used\n", swap_total / 1024.0,
           swap_free / 1024.0, swap_used / 1024.0);
    printf("\n%6s %-8s %c %10s %s\n", "PID", "USER", 'S', "MEM(KB)", "COMMAND");

    int maxrows = interactive && rows > 7 ? rows - 7 : n;
    char userbuf[64];
    for (int i = 0; i < n && i < maxrows; i++) {
        printf("%6u %-8s %c %10lu %s\n", procs[i].pid,
               lookup_user(procs[i].uid, userbuf, sizeof(userbuf)), procs[i].state, procs[i].mem_kb,
               procs[i].exe);
    }

    free(procs);
    free(uptime);
    free(loadavg);
    free(meminfo);
    fflush(stdout);
}

int main(int argc, char **argv) {
    kx_prog = "top";
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == 0) break;
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strncmp(a, "-d", 2) == 0) {
            const char *val = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!val) kx_die("usage: top [-d SECONDS] [-n COUNT] [-p PID] [-b]");
            double secs = strtod(val, NULL);
            if (secs < 0) secs = 0;
            delay_ms = (int) (secs * 1000);
            continue;
        }
        if (strncmp(a, "-n", 2) == 0) {
            const char *val = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!val) kx_die("usage: top [-d SECONDS] [-n COUNT] [-p PID] [-b]");
            iterations = atoi(val);
            continue;
        }
        if (strncmp(a, "-p", 2) == 0) {
            const char *val = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!val) kx_die("usage: top [-d SECONDS] [-n COUNT] [-p PID] [-b]");
            filter_pid = (unsigned) strtoul(val, NULL, 10);
            continue;
        }
        if (strcmp(a, "-b") == 0) {
            batch = true;
            continue;
        }
        kx_die("usage: top [-d SECONDS] [-n COUNT] [-p PID] [-b]");
    }
    if (i < argc) kx_die("usage: top [-d SECONDS] [-n COUNT] [-p PID] [-b]");

    bool interactive = !batch && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, NULL);
    if (interactive) set_raw_mode();

    for (int iter = 0; (iterations < 0 || iter < iterations) && !stop_flag; iter++) {
        draw(interactive);
        if (interactive) {
            struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
            if (poll(&pfd, 1, delay_ms) > 0) {
                char c;
                if (read(STDIN_FILENO, &c, 1) == 1 && (c == 'q' || c == 'Q')) break;
            }
        } else if (!stop_flag) {
            poll(NULL, 0, delay_ms);
        }
    }

    restore_term();
    return 0;
}
