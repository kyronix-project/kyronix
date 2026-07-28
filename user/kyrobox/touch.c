#define _XOPEN_SOURCE 700
#include "common.h"
#include <utime.h>

typedef struct {
    bool access_only;
    bool modify_only;
    bool no_create;
    bool no_dereference;
    bool have_time;
    struct timespec time;
} touch_options_t;

static bool parse_epoch(const char *value, struct timespec *result) {
    if (value[0] != '@') return false;
    char *end = NULL;
    double seconds = strtod(value + 1, &end);
    if (end == value + 1 || *end || seconds < 0) return false;
    result->tv_sec = (time_t) seconds;
    result->tv_nsec = (long) ((seconds - (double) result->tv_sec) * 1000000000.0);
    return true;
}

static bool parse_date(const char *value, struct timespec *result) {
    if (parse_epoch(value, result)) return true;
    struct tm tm = {0};
    char *end = strptime(value, "%Y-%m-%d %H:%M:%S", &tm);
    if (!end || *end) {
        memset(&tm, 0, sizeof(tm));
        end = strptime(value, "%Y-%m-%d", &tm);
    }
    if (!end || *end) return false;
    tm.tm_isdst = -1;
    time_t seconds = mktime(&tm);
    if (seconds == (time_t) -1) return false;
    result->tv_sec = seconds;
    result->tv_nsec = 0;
    return true;
}

static bool parse_stamp(const char *value, struct timespec *result) {
    char digits[16];
    const char *dot = strchr(value, '.');
    size_t len = dot ? (size_t) (dot - value) : strlen(value);
    if (len != 8 && len != 10 && len != 12) return false;
    if (len >= sizeof(digits)) return false;
    memcpy(digits, value, len);
    digits[len] = '\0';
    for (size_t i = 0; i < len; i++)
        if (!isdigit((unsigned char) digits[i])) return false;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    size_t pos = 0;
    if (len == 12) {
        char year[5];
        memcpy(year, digits, 4);
        year[4] = '\0';
        tm.tm_year = atoi(year) - 1900;
        pos = 4;
    } else if (len == 10) {
        char year[3];
        memcpy(year, digits, 2);
        year[2] = '\0';
        int y = atoi(year);
        tm.tm_year = (y >= 69 ? 1900 + y : 2000 + y) - 1900;
        pos = 2;
    }
    char part[3] = {0};
    memcpy(part, digits + pos, 2);
    tm.tm_mon = atoi(part) - 1;
    memcpy(part, digits + pos + 2, 2);
    tm.tm_mday = atoi(part);
    memcpy(part, digits + pos + 4, 2);
    tm.tm_hour = atoi(part);
    memcpy(part, digits + pos + 6, 2);
    tm.tm_min = atoi(part);
    tm.tm_sec = dot ? atoi(dot + 1) : 0;
    tm.tm_isdst = -1;

    time_t seconds = mktime(&tm);
    if (seconds == (time_t) -1 || tm.tm_mon < 0 || tm.tm_mon > 11 ||
        tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_hour > 23 ||
        tm.tm_min > 59 || tm.tm_sec > 60)
        return false;
    result->tv_sec = seconds;
    result->tv_nsec = 0;
    return true;
}

int main(int argc, char **argv) {
    kx_prog = "touch";
    touch_options_t o = {0};
    const char *reference = NULL;
    int first = 1;

    for (; first < argc; first++) {
        const char *arg = argv[first];
        if (strcmp(arg, "--") == 0) {
            first++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') break;
        if (strcmp(arg, "--help") == 0) {
            puts("usage: touch [-acmh] [-d DATE|-t STAMP|-r FILE] FILE...");
            return 0;
        }
        if ((strcmp(arg, "-d") == 0 || strcmp(arg, "-t") == 0 ||
             strcmp(arg, "-r") == 0) && first + 1 >= argc)
            kx_die("option requires an argument");
        if (strcmp(arg, "-d") == 0) {
            if (!parse_date(argv[++first], &o.time)) kx_die("invalid date");
            o.have_time = true;
        } else if (strcmp(arg, "-t") == 0) {
            if (!parse_stamp(argv[++first], &o.time)) kx_die("invalid timestamp");
            o.have_time = true;
        } else if (strcmp(arg, "-r") == 0) {
            reference = argv[++first];
        } else {
            for (const char *p = arg + 1; *p; p++) {
                switch (*p) {
                case 'a': o.access_only = true; break;
                case 'c': o.no_create = true; break;
                case 'h': o.no_dereference = true; break;
                case 'm': o.modify_only = true; break;
                default: kx_die("invalid option");
                }
            }
        }
    }
    if (first == argc) kx_die("missing file operand");
    if (reference && o.have_time) kx_die("cannot combine -r with -d or -t");

    struct stat reference_st;
    if (reference && stat(reference, &reference_st) < 0) {
        kx_warn(reference);
        return 1;
    }
    int rc = 0;
    for (int i = first; i < argc; i++) {
        struct stat st;
        int stat_flags = o.no_dereference ? AT_SYMLINK_NOFOLLOW : 0;
        if ((o.no_dereference ? lstat(argv[i], &st) : stat(argv[i], &st)) < 0) {
            if (errno != ENOENT || o.no_create) {
                if (errno != ENOENT) {
                    kx_warn(argv[i]);
                    rc = 1;
                }
                continue;
            }
            int fd = open(argv[i], O_WRONLY | O_CREAT | O_EXCL, 0666);
            if (fd < 0) {
                kx_warn(argv[i]);
                rc = 1;
                continue;
            }
            close(fd);
            if (stat(argv[i], &st) < 0) {
                kx_warn(argv[i]);
                rc = 1;
                continue;
            }
        }

        struct timespec times[2] = { st.st_atim, st.st_mtim };
        if (reference) {
            times[0] = reference_st.st_atim;
            times[1] = reference_st.st_mtim;
        } else if (o.have_time) {
            times[0] = times[1] = o.time;
        } else {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            times[0] = times[1] = now;
        }
        if (o.access_only && !o.modify_only) times[1] = st.st_mtim;
        if (o.modify_only && !o.access_only) times[0] = st.st_atim;
        if (utimensat(AT_FDCWD, argv[i], times, stat_flags) < 0) {
            int saved = errno;
            struct utimbuf legacy = {
                .actime = times[0].tv_sec,
                .modtime = times[1].tv_sec,
            };
            if (o.no_dereference || utime(argv[i], &legacy) < 0) {
                errno = saved;
                kx_warn(argv[i]);
                rc = 1;
            }
        }
    }
    return rc;
}
