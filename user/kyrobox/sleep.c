#include "common.h"

static double parse_duration(const char *value) {
    char *end = NULL;
    errno = 0;
    double amount = strtod(value, &end);
    if (errno || end == value || amount < 0) kx_die("invalid time interval");

    double scale = 1.0;
    if (*end != '\0') {
        if (end[1] != '\0') kx_die("invalid time suffix");
        switch (*end) {
        case 's': scale = 1.0; break;
        case 'm': scale = 60.0; break;
        case 'h': scale = 3600.0; break;
        case 'd': scale = 86400.0; break;
        default: kx_die("invalid time suffix");
        }
    }
    return amount * scale;
}

int main(int argc, char **argv) {
    kx_prog = "sleep";
    if (argc < 2) kx_die("missing operand");
    if (strcmp(argv[1], "--help") == 0) {
        puts("usage: sleep NUMBER[smhd]...");
        return 0;
    }

    double total = 0.0;
    for (int i = 1; i < argc; i++) total += parse_duration(argv[i]);
    if (total > (double) LONG_MAX) kx_die("time interval too large");

    struct timespec request = {
        .tv_sec = (time_t) total,
        .tv_nsec = (long) ((total - (double) (time_t) total) * 1000000000.0),
    };
    while (nanosleep(&request, &request) < 0) {
        if (errno != EINTR) {
            kx_warn("nanosleep");
            return 1;
        }
    }
    return 0;
}
