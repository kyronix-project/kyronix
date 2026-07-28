#include "common.h"

int main(int argc, char **argv) {
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        puts("usage: yes [STRING]...");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        puts("yes (Kyrobox)");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--") == 0) first++;

    size_t length = 1;
    if (first == argc) {
        length++;
    } else {
        for (int i = first; i < argc; i++) {
            if (SIZE_MAX - length <= strlen(argv[i]) + 1) return 1;
            length += strlen(argv[i]) + (i + 1 < argc ? 1U : 0U);
        }
    }
    char *line = malloc(length);
    if (!line) return 1;
    char *p = line;
    if (first == argc) {
        *p++ = 'y';
    } else {
        for (int i = first; i < argc; i++) {
            size_t n = strlen(argv[i]);
            memcpy(p, argv[i], n);
            p += n;
            if (i + 1 < argc) *p++ = ' ';
        }
    }
    *p++ = '\n';

    while (fwrite(line, 1, (size_t) (p - line), stdout) == (size_t) (p - line)) {
    }
    free(line);
    return ferror(stdout) ? 1 : 0;
}
