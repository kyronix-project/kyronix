#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void usage(void) {
    fprintf(stderr, "usage: logger [-t tag] message...\n");
    fprintf(stderr, "       logger [-t tag] < /dev/stdin\n");
    fprintf(stderr, "  -t tag    use specified tag (default: logger)\n");
    fprintf(stderr, "  -h        show help\n");
}

int main(int argc, char **argv) {
    char tag[32] = "logger";
    int first = 1;

    if (first < argc && strcmp(argv[first], "-h") == 0) {
        usage();
        return 0;
    }

    if (first < argc && strcmp(argv[first], "-t") == 0 && first + 1 < argc) {
        strncpy(tag, argv[first + 1], sizeof(tag) - 1);
        first += 2;
    }

    FILE *log = fopen("/var/log/messages", "a");
    if (!log) {
        fprintf(stderr, "logger: %s: %s\n", "/var/log/messages", strerror(errno));
        return 1;
    }

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[20];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);

    if (first >= argc) {
        if (isatty(STDIN_FILENO)) {
            usage();
            fclose(log);
            return 1;
        }
    }

    if (first < argc) {
        fprintf(log, "%s [%s]", ts, tag);
        for (int i = first; i < argc; i++)
            fprintf(log, " %s", argv[i]);
        fprintf(log, "\n");
    } else {
        char buf[4096];
        ssize_t n;
        while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
            char *p = buf;
            ssize_t remain = n;
            while (remain > 0) {
                char *nl = memchr(p, '\n', (size_t)remain);
                size_t len = nl ? (size_t)(nl - p) : (size_t)remain;
                char saved = p[len];
                p[len] = 0;
                fprintf(log, "%s [%s] %s\n", ts, tag, p);
                p[len] = saved;
                if (nl) { p = nl + 1; remain -= (len + 1); }
                else break;
            }
        }
    }

    fclose(log);
    return 0;
}
