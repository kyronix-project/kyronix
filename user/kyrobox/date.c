#define _XOPEN_SOURCE 700
#include "common.h"

static bool parse_time(const char *value, time_t *result) {
    if (value[0] == '@') {
        char *end = NULL;
        long long seconds = strtoll(value + 1, &end, 10);
        if (end == value + 1 || *end) return false;
        *result = (time_t) seconds;
        return true;
    }

    const char *formats[] = {
        "%Y-%m-%d %H:%M:%S",
        "%Y-%m-%d %H:%M",
        "%Y-%m-%d",
        "%H:%M:%S",
        NULL,
    };
    time_t now = time(NULL);
    for (int i = 0; formats[i]; i++) {
        struct tm tm;
        localtime_r(&now, &tm);
        char *end = strptime(value, formats[i], &tm);
        if (!end || *end) continue;
        tm.tm_isdst = -1;
        time_t parsed = mktime(&tm);
        if (parsed == (time_t) -1) return false;
        *result = parsed;
        return true;
    }
    return false;
}

int main(int argc, char **argv) {
    kx_prog = "date";
    bool utc = false;
    bool set_clock = false;
    const char *format = NULL;
    const char *date_value = NULL;
    const char *reference = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--") == 0) {
            if (++i < argc) format = argv[i];
            if (i + 1 < argc) kx_die("extra operand");
            break;
        }
        if (strcmp(arg, "-u") == 0 || strcmp(arg, "--utc") == 0) {
            utc = true;
        } else if ((strcmp(arg, "-d") == 0 || strcmp(arg, "--date") == 0) &&
                   i + 1 < argc) {
            date_value = argv[++i];
        } else if (strcmp(arg, "-r") == 0 && i + 1 < argc) {
            reference = argv[++i];
        } else if (strcmp(arg, "-s") == 0 && i + 1 < argc) {
            date_value = argv[++i];
            set_clock = true;
        } else if (strcmp(arg, "-R") == 0) {
            format = "%a, %d %b %Y %H:%M:%S %z";
        } else if (strcmp(arg, "-I") == 0 || strcmp(arg, "--iso-8601") == 0) {
            format = "%Y-%m-%d";
        } else if (strcmp(arg, "-Is") == 0 || strcmp(arg, "--iso-8601=seconds") == 0) {
            format = "%Y-%m-%dT%H:%M:%S%z";
        } else if (strcmp(arg, "--help") == 0) {
            puts("usage: date [-uR] [-I|-Is] [-d DATE] [-r FILE] [-s DATE] [+FORMAT]");
            return 0;
        } else if (arg[0] == '+') {
            format = arg + 1;
        } else {
            kx_die("invalid argument");
        }
    }
    if (date_value && reference) kx_die("cannot combine -d/-s with -r");

    time_t value;
    if (reference) {
        struct stat st;
        if (stat(reference, &st) < 0) {
            kx_warn(reference);
            return 1;
        }
        value = st.st_mtime;
    } else if (date_value) {
        if (!parse_time(date_value, &value)) kx_die("invalid date");
    } else {
        value = time(NULL);
    }

    if (set_clock) {
        struct timespec ts = { .tv_sec = value, .tv_nsec = 0 };
        if (clock_settime(CLOCK_REALTIME, &ts) < 0) {
            kx_warn("clock_settime");
            return 1;
        }
    }

    struct tm tm;
    if (utc)
        gmtime_r(&value, &tm);
    else
        localtime_r(&value, &tm);
    if (!format) format = "%a %b %e %H:%M:%S %Z %Y";

    char output[1024];
    size_t n = strftime(output, sizeof(output), format, &tm);
    if (n == 0 && format[0]) kx_die("formatted value is too long");
    puts(output);
    return 0;
}
