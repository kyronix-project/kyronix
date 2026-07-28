#include "common.h"

enum {
    F_SYSNAME = 1 << 0,
    F_NODENAME = 1 << 1,
    F_RELEASE = 1 << 2,
    F_VERSION = 1 << 3,
    F_MACHINE = 1 << 4,
    F_PROCESSOR = 1 << 5,
    F_HARDWARE = 1 << 6,
    F_OS = 1 << 7
};

static void usage(void) {
    puts("usage: uname [-asnrvmpio]");
}

static void add_field(const char *value, bool *first) {
    if (!*first) putchar(' ');
    fputs(value, stdout);
    *first = false;
}

int main(int argc, char **argv) {
    kx_prog = "uname";
    unsigned flags = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) {
            usage();
            return 0;
        }
        if (strcmp(a, "--") == 0) {
            if (++i != argc) kx_die("extra operand");
            break;
        }
        if (a[0] != '-' || a[1] == '\0') kx_die("extra operand");
        for (const char *p = a + 1; *p; p++) {
            if (*p == 'a') flags |= 0xffU;
            else if (*p == 's') flags |= F_SYSNAME;
            else if (*p == 'n') flags |= F_NODENAME;
            else if (*p == 'r') flags |= F_RELEASE;
            else if (*p == 'v') flags |= F_VERSION;
            else if (*p == 'm') flags |= F_MACHINE;
            else if (*p == 'p') flags |= F_PROCESSOR;
            else if (*p == 'i') flags |= F_HARDWARE;
            else if (*p == 'o') flags |= F_OS;
            else {
                fprintf(stderr, "uname: invalid option -- '%c'\n", *p);
                return 1;
            }
        }
    }
    if (!flags) flags = F_SYSNAME;
    struct utsname u;
    if (uname(&u) < 0) {
        kx_warn("uname");
        return 1;
    }
    bool first = true;
    if (flags & F_SYSNAME) add_field(u.sysname, &first);
    if (flags & F_NODENAME) add_field(u.nodename, &first);
    if (flags & F_RELEASE) add_field(u.release, &first);
    if (flags & F_VERSION) add_field(u.version, &first);
    if (flags & F_MACHINE) add_field(u.machine, &first);
    if (flags & F_PROCESSOR) add_field(u.machine[0] ? u.machine : "unknown", &first);
    if (flags & F_HARDWARE) add_field(u.machine[0] ? u.machine : "unknown", &first);
    if (flags & F_OS) add_field("Kyronix", &first);
    putchar('\n');
    return ferror(stdout) ? 1 : 0;
}
