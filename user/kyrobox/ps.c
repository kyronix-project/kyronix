#include "common.h"

typedef struct {
    unsigned pid;
    unsigned ppid;
    unsigned uid;
    unsigned long mem_kb;
    char state;
    char command[512];
} process_t;

typedef enum {
    COL_PID,
    COL_PPID,
    COL_UID,
    COL_USER,
    COL_STATE,
    COL_MEM,
    COL_VSZ,
    COL_RSS,
    COL_COMM,
    COL_COMMAND
} column_t;

static column_t columns[16];
static size_t ncolumns;
static unsigned selected_pids[128];
static size_t npids;
static bool filter_uid;
static unsigned selected_uid;
static bool no_headers;
static bool deselect;
static const char *sort_key = "pid";
static bool reverse_sort;

static void usage(void) {
    puts("usage: ps [-Aeax] [-f|-l|-u] [-p PID,...] [-U USER] [-o FIELD,...] [--sort FIELD]");
}

static bool parse_unsigned(const char *s, unsigned *value) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (end == s || *end || errno == ERANGE || n > UINT_MAX) return false;
    *value = (unsigned) n;
    return true;
}

static void parse_pid_list(const char *list) {
    char *copy = strdup(list);
    if (!copy) kx_die("out of memory");
    char *save = NULL;
    for (char *p = strtok_r(copy, ", ", &save); p; p = strtok_r(NULL, ", ", &save)) {
        if (npids == sizeof(selected_pids) / sizeof(selected_pids[0]) ||
            !parse_unsigned(p, &selected_pids[npids]))
            kx_die("invalid process ID list");
        npids++;
    }
    free(copy);
}

static column_t parse_column(const char *name) {
    if (strcasecmp(name, "pid") == 0) return COL_PID;
    if (strcasecmp(name, "ppid") == 0) return COL_PPID;
    if (strcasecmp(name, "uid") == 0) return COL_UID;
    if (strcasecmp(name, "user") == 0) return COL_USER;
    if (strcasecmp(name, "s") == 0 || strcasecmp(name, "stat") == 0 ||
        strcasecmp(name, "state") == 0)
        return COL_STATE;
    if (strcasecmp(name, "mem") == 0) return COL_MEM;
    if (strcasecmp(name, "vsz") == 0) return COL_VSZ;
    if (strcasecmp(name, "rss") == 0) return COL_RSS;
    if (strcasecmp(name, "comm") == 0) return COL_COMM;
    if (strcasecmp(name, "command") == 0 || strcasecmp(name, "args") == 0 ||
        strcasecmp(name, "cmd") == 0)
        return COL_COMMAND;
    kx_die("unknown output field");
    return COL_PID;
}

static void parse_columns(const char *list) {
    char *copy = strdup(list);
    if (!copy) kx_die("out of memory");
    char *save = NULL;
    for (char *p = strtok_r(copy, ", ", &save); p; p = strtok_r(NULL, ", ", &save)) {
        char *equals = strchr(p, '=');
        if (equals) *equals = '\0';
        if (ncolumns == sizeof(columns) / sizeof(columns[0])) kx_die("too many output fields");
        columns[ncolumns++] = parse_column(p);
    }
    free(copy);
}

static int compare_processes(const void *va, const void *vb) {
    const process_t *a = va, *b = vb;
    int result;
    if (strcmp(sort_key, "pid") == 0) result = (a->pid > b->pid) - (a->pid < b->pid);
    else if (strcmp(sort_key, "ppid") == 0) result = (a->ppid > b->ppid) - (a->ppid < b->ppid);
    else if (strcmp(sort_key, "uid") == 0) result = (a->uid > b->uid) - (a->uid < b->uid);
    else if (strcmp(sort_key, "rss") == 0 || strcmp(sort_key, "vsz") == 0 ||
             strcmp(sort_key, "mem") == 0)
        result = (a->mem_kb > b->mem_kb) - (a->mem_kb < b->mem_kb);
    else if (strcmp(sort_key, "comm") == 0 || strcmp(sort_key, "command") == 0)
        result = strcmp(a->command, b->command);
    else
        result = 0;
    return reverse_sort ? -result : result;
}

static bool selected(const process_t *p) {
    bool match = true;
    if (npids) {
        match = false;
        for (size_t i = 0; i < npids; i++)
            if (p->pid == selected_pids[i]) match = true;
    }
    if (filter_uid && p->uid != selected_uid) match = false;
    return deselect ? !match : match;
}

static const char *column_header(column_t c) {
    static const char *const names[] = {
        "PID", "PPID", "UID", "USER", "S", "MEM(KB)", "VSZ", "RSS", "COMMAND", "COMMAND"
    };
    return names[c];
}

static void print_column(column_t c, const process_t *p) {
    struct passwd *pw;
    switch (c) {
        case COL_PID: printf("%5u", p->pid); break;
        case COL_PPID: printf("%5u", p->ppid); break;
        case COL_UID: printf("%5u", p->uid); break;
        case COL_USER:
            pw = getpwuid((uid_t) p->uid);
            printf("%-8.8s", pw ? pw->pw_name : "?");
            break;
        case COL_STATE: printf("%c", p->state); break;
        case COL_MEM:
        case COL_VSZ:
        case COL_RSS: printf("%8lu", p->mem_kb); break;
        case COL_COMM: printf("%s", kx_base(p->command)); break;
        case COL_COMMAND: printf("%s", p->command); break;
    }
}

int main(int argc, char **argv) {
    kx_prog = "ps";
    bool full = false, long_format = false, user_format = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) {
            usage();
            return 0;
        }
        if (strcmp(a, "--no-headers") == 0) {
            no_headers = true;
            continue;
        }
        if (strncmp(a, "--sort=", 7) == 0) {
            sort_key = a + 7;
            if (*sort_key == '-') {
                reverse_sort = true;
                sort_key++;
            }
            continue;
        }
        if ((strcmp(a, "-p") == 0 || strcmp(a, "--pid") == 0 ||
             strcmp(a, "-q") == 0) && i + 1 < argc) {
            parse_pid_list(argv[++i]);
            continue;
        }
        if ((strcmp(a, "-U") == 0 || strcmp(a, "--User") == 0) && i + 1 < argc) {
            const char *user = argv[++i];
            unsigned uid;
            if (parse_unsigned(user, &uid)) selected_uid = uid;
            else {
                struct passwd *pw = getpwnam(user);
                if (!pw) kx_die("unknown user");
                selected_uid = pw->pw_uid;
            }
            filter_uid = true;
            continue;
        }
        if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            parse_columns(argv[++i]);
            continue;
        }
        const char *opts = a;
        if (*opts == '-') opts++;
        if (!*opts) kx_die("invalid option");
        for (const char *p = opts; *p; p++) {
            if (*p == 'A' || *p == 'e' || *p == 'a' || *p == 'x') {
                continue;
            } else if (*p == 'f') {
                full = true;
            } else if (*p == 'l') {
                long_format = true;
            } else if (*p == 'u') {
                user_format = true;
            } else if (*p == 'N') {
                deselect = true;
            } else {
                fprintf(stderr, "ps: invalid option -- '%c'\n", *p);
                return 1;
            }
        }
    }

    if (!ncolumns) {
        if (user_format) {
            column_t defaults[] = { COL_USER, COL_PID, COL_MEM, COL_STATE, COL_COMMAND };
            memcpy(columns, defaults, sizeof(defaults));
            ncolumns = sizeof(defaults) / sizeof(defaults[0]);
        } else if (full) {
            column_t defaults[] = { COL_UID, COL_PID, COL_PPID, COL_STATE, COL_MEM, COL_COMMAND };
            memcpy(columns, defaults, sizeof(defaults));
            ncolumns = sizeof(defaults) / sizeof(defaults[0]);
        } else if (long_format) {
            column_t defaults[] = { COL_PID, COL_PPID, COL_STATE, COL_UID, COL_MEM, COL_COMMAND };
            memcpy(columns, defaults, sizeof(defaults));
            ncolumns = sizeof(defaults) / sizeof(defaults[0]);
        } else {
            column_t defaults[] = { COL_PID, COL_PPID, COL_STATE, COL_COMMAND };
            memcpy(columns, defaults, sizeof(defaults));
            ncolumns = sizeof(defaults) / sizeof(defaults[0]);
        }
    }

    FILE *f = fopen("/proc/pids", "r");
    if (!f) {
        kx_warn("/proc/pids");
        return 1;
    }
    process_t processes[256];
    size_t count = 0;
    char line[1024];
    while (count < sizeof(processes) / sizeof(processes[0]) && fgets(line, sizeof(line), f)) {
        process_t p = { 0 };
        if (sscanf(line, "%u %u %c %u %lu %511s", &p.pid, &p.ppid, &p.state, &p.uid,
                   &p.mem_kb, p.command) >= 5 && selected(&p))
            processes[count++] = p;
    }
    fclose(f);
    qsort(processes, count, sizeof(processes[0]), compare_processes);

    if (!no_headers) {
        for (size_t c = 0; c < ncolumns; c++) {
            if (c) putchar(' ');
            printf("%s", column_header(columns[c]));
        }
        putchar('\n');
    }
    for (size_t n = 0; n < count; n++) {
        for (size_t c = 0; c < ncolumns; c++) {
            if (c) putchar(' ');
            print_column(columns[c], &processes[n]);
        }
        putchar('\n');
    }
    return ferror(stdout) ? 1 : 0;
}
