#define _GNU_SOURCE
#include "common.h"

#include <sys/wait.h>

#define SYS_jail_create 500
#define SYS_jail_attach 501
#define SYS_jail_get 502
#define SYS_jail_list 503
#define SYS_jail_remove 504
#define SYS_jail_self 505
#define SYS_jail_set_auto 506

#define JAIL_NAME_MAX 32
#define JAIL_ROOT_MAX 256
#define JAILF_FS 0x01u
#define JAILF_PID 0x02u
#define JAILF_IPC 0x04u
#define JAILF_PRIV 0x08u
#define JAILF_ALL (JAILF_FS | JAILF_PID | JAILF_IPC | JAILF_PRIV)

typedef struct {
    char root[JAIL_ROOT_MAX];
    char name[JAIL_NAME_MAX];
    uint32_t flags;
    uint32_t max_procs;
    uint32_t attach;
} kjail_conf_t;

typedef struct {
    uint32_t id;
    uint32_t parent_id;
    uint32_t flags;
    uint32_t nprocs;
    uint32_t max_procs;
    uint32_t creator_uid;
    char name[JAIL_NAME_MAX];
    char root[JAIL_ROOT_MAX];
} kjail_info_t;

static volatile sig_atomic_t child_pid = -1;

static void usage(FILE *stream) {
    fprintf(stream,
        "usage:\n"
        "  jailctl create [-n name] [-r root] [-f flags] [-p max-procs]\n"
        "  jailctl exec   [-n name] [-r root] [-f flags] [-p max-procs] -- command [args...]\n"
        "  jailctl attach ID -- command [args...]\n"
        "  jailctl list\n"
        "  jailctl show ID\n"
        "  jailctl remove ID\n"
        "  jailctl self\n"
        "  jailctl auto on|off\n"
        "\n"
        "  (flags: all, none, or a comma-separated list of fs,pid,ipc,priv)\n"
        "  (defaults: root=/, flags=all, max-procs=unlimited              )\n");
}

static int parse_u32(const char *text, uint32_t *value, int allow_zero) {
    if (!text || !*text || text[0] == '-') return -1;
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || !end || *end || parsed > UINT32_MAX || (!allow_zero && parsed == 0))
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int parse_flags(const char *text, uint32_t *flags) {
    if (strcmp(text, "all") == 0) {
        *flags = JAILF_ALL;
        return 0;
    }
    if (strcmp(text, "none") == 0) {
        *flags = 0;
        return 0;
    }

    if (strlen(text) >= 128) return -1;
    char copy[128];
    strcpy(copy, text);
    uint32_t parsed = 0;
    char *save = NULL;
    for (char *part = strtok_r(copy, ",", &save); part;
         part = strtok_r(NULL, ",", &save)) {
        if (strcmp(part, "fs") == 0)
            parsed |= JAILF_FS;
        else if (strcmp(part, "pid") == 0)
            parsed |= JAILF_PID;
        else if (strcmp(part, "ipc") == 0)
            parsed |= JAILF_IPC;
        else if (strcmp(part, "priv") == 0)
            parsed |= JAILF_PRIV;
        else
            return -1;
    }
    if (parsed == 0) return -1;
    *flags = parsed;
    return 0;
}

static void flags_string(uint32_t flags, char *out, size_t size) {
    if (flags == 0) {
        snprintf(out, size, "none");
        return;
    }
    out[0] = '\0';
    struct {
        uint32_t flag;
        const char *name;
    } names[] = {
        {JAILF_FS, "fs"}, {JAILF_PID, "pid"},
        {JAILF_IPC, "ipc"}, {JAILF_PRIV, "priv"},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(flags & names[i].flag)) continue;
        size_t used = strlen(out);
        snprintf(out + used, size - used, "%s%s", used ? "," : "", names[i].name);
    }
}

static long jail_create_call(kjail_conf_t *conf) {
    return syscall(SYS_jail_create, conf);
}

static long jail_get_call(uint32_t id, kjail_info_t *info) {
    return syscall(SYS_jail_get, id, info);
}

static void print_info(const kjail_info_t *info) {
    char flags[64];
    flags_string(info->flags, flags, sizeof(flags));
    printf("%-4u %-4u %-6u %-6u %-5u %-17s %-31s %s\n",
           info->id, info->parent_id, info->nprocs, info->max_procs,
           info->creator_uid, flags, info->name[0] ? info->name : "-",
           info->root[0] ? info->root : "/");
}

static int show_jail(uint32_t id) {
    kjail_info_t info;
    if (jail_get_call(id, &info) < 0) {
        kx_warn("jail_get");
        return 1;
    }
    puts("ID   PARENT PROCS  MAX    UID   FLAGS             NAME                            ROOT");
    print_info(&info);
    return 0;
}

static int list_jails(void) {
    long count = syscall(SYS_jail_list, NULL, 0);
    if (count < 0) {
        kx_warn("jail_list");
        return 1;
    }
    if (count == 0) {
        puts("No jails.");
        return 0;
    }
    if ((unsigned long)count > SIZE_MAX / sizeof(uint32_t)) {
        errno = EOVERFLOW;
        kx_warn("jail_list");
        return 1;
    }
    uint32_t *ids = calloc((size_t)count, sizeof(*ids));
    if (!ids) {
        kx_warn("calloc");
        return 1;
    }
    long current = syscall(SYS_jail_list, ids, count);
    if (current < 0) {
        kx_warn("jail_list");
        free(ids);
        return 1;
    }
    long available = current < count ? current : count;
    puts("ID   PARENT PROCS  MAX    UID   FLAGS             NAME                            ROOT");
    for (long i = 0; i < available; i++) {
        kjail_info_t info;
        if (jail_get_call(ids[i], &info) == 0) print_info(&info);
    }
    free(ids);
    return 0;
}

static int validate_root(const kjail_conf_t *conf) {
    if (!(conf->flags & JAILF_FS)) return 0;
    struct stat st;
    if (stat(conf->root, &st) < 0) {
        kx_warn(conf->root);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "jailctl: %s: not a directory\n", conf->root);
        return -1;
    }
    return 0;
}

static int parse_config(int argc, char **argv, int start, kjail_conf_t *conf,
                        int require_command, int *command_index) {
    memset(conf, 0, sizeof(*conf));
    strcpy(conf->root, "/");
    strcpy(conf->name, "jail");
    conf->flags = JAILF_ALL;

    int i = start;
    while (i < argc) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            if (strlen(argv[i + 1]) >= sizeof(conf->name)) {
                fprintf(stderr, "jailctl: jail name is too long\n");
                return -1;
            }
            strcpy(conf->name, argv[i + 1]);
            i += 2;
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            if (strlen(argv[i + 1]) >= sizeof(conf->root)) {
                fprintf(stderr, "jailctl: jail root is too long\n");
                return -1;
            }
            strcpy(conf->root, argv[i + 1]);
            i += 2;
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            if (parse_flags(argv[i + 1], &conf->flags) < 0) {
                fprintf(stderr, "jailctl: invalid flags: %s\n", argv[i + 1]);
                return -1;
            }
            i += 2;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            if (parse_u32(argv[i + 1], &conf->max_procs, 1) < 0) {
                fprintf(stderr, "jailctl: invalid process limit: %s\n", argv[i + 1]);
                return -1;
            }
            i += 2;
        } else {
            fprintf(stderr, "jailctl: invalid option: %s\n", argv[i]);
            return -1;
        }
    }
    if (require_command && i >= argc) {
        fprintf(stderr, "jailctl: missing command after --\n");
        return -1;
    }
    if (!require_command && i != argc) {
        fprintf(stderr, "jailctl: unexpected command\n");
        return -1;
    }
    if (validate_root(conf) < 0) return -1;
    if (command_index) *command_index = i;
    return 0;
}

static void close_extra_fds(void) {
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit < 0 || limit > 65536) limit = 65536;
    for (int fd = 3; fd < limit; fd++) close(fd);
}

static void exec_in_jail(uint32_t id, char **command) {
    if (syscall(SYS_jail_attach, id) < 0) {
        kx_warn("jail_attach");
        _exit(126);
    }
    close_extra_fds();
    if (chdir("/") < 0) {
        kx_warn("/");
        _exit(126);
    }
    execvp(command[0], command);
    int exit_status = errno == ENOENT ? 127 : 126;
    kx_warn(command[0]);
    _exit(exit_status);
}

static void forward_signal(int signal_number) {
    pid_t pid = (pid_t)child_pid;
    if (pid > 0) kill(pid, signal_number);
}

static int wait_and_remove(pid_t pid, uint32_t id) {
    child_pid = pid;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = forward_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        kx_warn("waitpid");
        status = 126 << 8;
        break;
    }
    child_pid = -1;
    if (syscall(SYS_jail_remove, id) < 0) kx_warn("jail_remove");
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 126;
}

static int create_or_exec(int argc, char **argv, int execute) {
    kjail_conf_t conf;
    int command_index = argc;
    if (parse_config(argc, argv, 2, &conf, execute, &command_index) < 0) return 2;

    long created = jail_create_call(&conf);
    if (created < 0) {
        kx_warn("jail_create");
        return 1;
    }
    uint32_t id = (uint32_t)created;
    if (!execute) {
        printf("%u\n", id);
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        kx_warn("fork");
        syscall(SYS_jail_remove, id);
        return 1;
    }
    if (pid == 0) exec_in_jail(id, &argv[command_index]);
    return wait_and_remove(pid, id);
}

static int attach_command(int argc, char **argv) {
    if (argc < 5 || strcmp(argv[3], "--") != 0) {
        fprintf(stderr, "jailctl: usage: jailctl attach ID -- command [args...]\n");
        return 2;
    }
    uint32_t id;
    if (parse_u32(argv[2], &id, 0) < 0) {
        fprintf(stderr, "jailctl: invalid jail ID: %s\n", argv[2]);
        return 2;
    }
    exec_in_jail(id, &argv[4]);
    return 126;
}

int main(int argc, char **argv) {
    kx_prog = "jailctl";
    if (argc < 2) {
        usage(stderr);
        return 2;
    }

    if (strcmp(argv[1], "create") == 0) return create_or_exec(argc, argv, 0);
    if (strcmp(argv[1], "exec") == 0) return create_or_exec(argc, argv, 1);
    if (strcmp(argv[1], "attach") == 0) return attach_command(argc, argv);
    if (strcmp(argv[1], "list") == 0 && argc == 2) return list_jails();
    if (strcmp(argv[1], "self") == 0 && argc == 2) {
        long id = syscall(SYS_jail_self);
        if (id < 0) {
            kx_warn("jail_self");
            return 1;
        }
        printf("%ld\n", id);
        return 0;
    }
    if ((strcmp(argv[1], "show") == 0 || strcmp(argv[1], "remove") == 0) && argc == 3) {
        uint32_t id;
        if (parse_u32(argv[2], &id, 0) < 0) {
            fprintf(stderr, "jailctl: invalid jail ID: %s\n", argv[2]);
            return 2;
        }
        if (strcmp(argv[1], "show") == 0) return show_jail(id);
        if (syscall(SYS_jail_remove, id) < 0) {
            kx_warn("jail_remove");
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "auto") == 0 && argc == 3) {
        int enabled;
        if (strcmp(argv[2], "on") == 0)
            enabled = 1;
        else if (strcmp(argv[2], "off") == 0)
            enabled = 0;
        else {
            fprintf(stderr, "jailctl: auto expects 'on' or 'off'\n");
            return 2;
        }
        if (syscall(SYS_jail_set_auto, enabled) < 0) {
            kx_warn("jail_set_auto");
            return 1;
        }
        return 0;
    }
    if ((strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) && argc == 2) {
        usage(stdout);
        return 0;
    }

    usage(stderr);
    return 2;
}
