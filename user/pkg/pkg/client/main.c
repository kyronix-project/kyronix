#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "pkg.h"

static int has_flag(int argc, char **argv, const char *name) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) return 1;
    }
    return 0;
}

static const char *first_nonflag(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] != '-') return argv[i];
    }
    return NULL;
}

static void usage(void) {
    fprintf(stderr,
        "%skx-pkg%s - package manager for kyronix. %sv" PKG_VERSION "%s\n\n"

        "%sUsage:%s\n"
        "  %spkg install%s <package>       Install a package\n"
        "  %spkg remove%s <package>        Uninstall a package\n"
        "  %spkg autoremove%s              Remove orphaned dependencies\n"
        "  %spkg list%s                    List installed packages\n"
        "\n"
        "%sRepository:%s\n"
        "  %spkg repo show%s               Show configured repositories\n"
        "  %spkg repo add%s <name> <url>   Add or update a repository\n"
        "  %spkg repo remove%s <name>      Remove a repository\n"
        "  %spkg repo ping%s               Check repository availability\n"
        "\n"
        "%sOptions:%s\n"
        "  %s-y, --yes%s                 Assume \"yes\" to all prompts\n"
        "  %s-v, --verbose%s               Verbose output\n"
        "  %s-h, --help%s                  Show this help\n"
        "\n",

        ANSI_CYAN, ANSI_RESET,
        ANSI_DIM, ANSI_RESET,

        ANSI_BOLD, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET,

        ANSI_BOLD, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET,

        ANSI_BOLD, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET,
        ANSI_CYAN, ANSI_RESET
    );
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *cmd = argv[1];
    verbose_mode = has_flag(argc - 2, argv + 2, "-verbose") || has_flag(argc - 2, argv + 2, "--verbose");
    yes_mode = has_flag(argc - 2, argv + 2, "-y") || has_flag(argc - 2, argv + 2, "--yes");

    if (strcmp(cmd, "repo") == 0) {
        const char *subcmd = (argc > 2) ? argv[2] : NULL;
        const char *arg = NULL;
        if (argc > 3) {
            static char argbuf[2048];
            argbuf[0] = '\0';
            for (int i = 3; i < argc; i++) {
                if (argv[i][0] == '-') continue;
                if (argbuf[0]) strcat(argbuf, " ");
                strcat(argbuf, argv[i]);
            }
            if (argbuf[0]) arg = argbuf;
        }
        cmd_repo(subcmd, arg);
        return 0;
    }

    if (strcmp(cmd, "install") == 0 || strcmp(cmd, "get") == 0) {
        const char *pkg = first_nonflag(argc - 2, argv + 2);
        if (!pkg) {
            fprintf(stderr, "%s%serror:%s package name required\n", ANSI_RED, ANSI_BOLD, ANSI_RESET);
            return 1;
        }
        cmd_get(pkg);
        return 0;
    }

    if (strcmp(cmd, "remove") == 0 || strcmp(cmd, "rm") == 0 || strcmp(cmd, "uninstall") == 0) {
        const char *pkg = first_nonflag(argc - 2, argv + 2);
        if (!pkg) {
            fprintf(stderr, "%s%serror:%s package name required\n", ANSI_RED, ANSI_BOLD, ANSI_RESET);
            return 1;
        }
        cmd_remove(pkg);
        return 0;
    }

    if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0) {
        cmd_list();
        return 0;
    }

    if (strcmp(cmd, "autoremove") == 0 || strcmp(cmd, "clean") == 0) {
        cmd_autoremove();
        return 0;
    }

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        fprintf(stdout, "kx-pkg %s\n", PKG_VERSION);
        return 0;
    }

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage();
        return 0;
    }

    fprintf(stderr, "%s%serror:%s unknown command '%s'\n", ANSI_RED, ANSI_BOLD, ANSI_RESET, cmd);
    usage();
    return 1;
}
