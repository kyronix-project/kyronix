#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
} __attribute__((packed));

static int getdents64_raw(int fd, void *buf, size_t count) {
    return syscall(217, fd, buf, count);
}


enum { DIAG_BUF = 2048 };

static int numeric(const char *s) {
    if (!*s) return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return 0;
    return 1;
}

static int getdents_loop(int fd, int maxcalls) {
    static char buf[DIAG_BUF];
    int total = 0;
    int pids_seen[256], np = 0;
    int dupes = 0;
    for (int call = 0; call < maxcalls; call++) {
        int r = getdents64_raw(fd, buf, sizeof(buf));
        if (r < 0) {
            fprintf(stderr, "  getdents64 call %d FAILED: %s\n", call, strerror(errno));
            return total;
        }
        if (r == 0) {
            fprintf(stderr, "  getdents64 call %d: EOF (0 bytes)\n", call);
            break;
        }
        int pos = 0, entries = 0, pids = 0;
        char names[128][24];
        while (pos < r) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + pos);
            if (d->d_reclen == 0) break;
            if (entries < 128) snprintf(names[entries], sizeof(names[0]), "%s", d->d_name);
            if (d->d_name[0] >= '1' && d->d_name[0] <= '9') {
                pids++;
                int p = atoi(d->d_name);
                if (np < 256) {
                    int isdup = 0;
                    for (int k = 0; k < np; k++) if (pids_seen[k] == p) isdup = 1;
                    if (isdup) dupes++;
                    else pids_seen[np++] = p;
                }
            }
            entries++;
            pos += d->d_reclen;
        }
        fprintf(stderr, "  call %d: %d bytes, %d entries (%d pid-dirs)", call, r, entries, pids);
        fprintf(stderr, " names:");
        for (int i = 0; i < entries && i < 128; i++) fprintf(stderr, " %s", names[i]);
        fprintf(stderr, "\n");
        total += entries;
    }
    fprintf(stderr, "  walk: total=%d unique_pids=%d dup_pid_hits=%d pids:", total, np, dupes);
    for (int k = 0; k < np && k < 64; k++) fprintf(stderr, " %d", pids_seen[k]);
    fprintf(stderr, "\n");
    return total;
}

static void probe_getdents_stability(void) {
    fprintf(stderr, "=== Probe 1: sequential getdents64(/proc), 2048B buffer, 5 calls ===\n");
    int fd = open("/proc", O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        fprintf(stderr, "  open /proc FAILED: %s\n", strerror(errno));
        return;
    }
    getdents_loop(fd, 16);
    close(fd);
    fprintf(stderr, "\n");
}

static long fast_atol(const char **loc) {
    const char *p = *loc;
    long v = 0;
    while (*p == ' ') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    while (*p == ' ') p++;
    *loc = p;
    return neg ? -v : v;
}

static unsigned long long fast_atoull(const char **loc) {
    const char *p = *loc;
    unsigned long long v = 0;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    while (*p == ' ') p++;
    *loc = p;
    return v;
}

static int advance_to_next(const char **loc) {
    const char *p = *loc;
    while (*p && *p != ' ') p++;
    if (!*p) return 0;
    *loc = p + 1;
    return 1;
}

/* faithful re-implementation of LinuxProcessTable_readStatFile */
static int htop_parse_stat(char *buf, unsigned long expect_pid) {
    const char *location = buf;
    if (expect_pid != (unsigned long) atoi(buf)) {
        fprintf(stderr, "    stat: pid mismatch (have %u want %lu)\n", atoi(buf), expect_pid);
        return 0;
    }
    char *space = strchr(buf, ' ');
    if (!space || !space[0] || !space[1]) { fprintf(stderr, "    stat: no comm\n"); return 0; }
    location = space + 2;
    char *end = strrchr(location, ')');
    if (!end || end < location) { fprintf(stderr, "    stat: no )\n"); return 0; }
    if (!end[0] || !end[1]) { fprintf(stderr, "    stat: no state\n"); return 0; }
    location = end + 2;                        /* state char, like htop */
    if (!location[0] || !location[1]) { fprintf(stderr, "    stat: state empty\n"); return 0; }
    location += 2;                             /* skip state and the space like htop */
    long ppid = fast_atol(&location);
    long pgrp = fast_atol(&location);
    long session = fast_atol(&location);
    unsigned long tty_nr = fast_atoull(&location);
    long tpgid = fast_atol(&location);
    unsigned long flags = fast_atoull(&location);
    unsigned long long minflt = fast_atoull(&location);
    unsigned long long cminflt = fast_atoull(&location);
    unsigned long long majflt = fast_atoull(&location);
    unsigned long long cmajflt = fast_atoull(&location);
    unsigned long long utime = fast_atoull(&location);
    unsigned long long stime = fast_atoull(&location);
    unsigned long long cutime = fast_atoull(&location);
    unsigned long long cstime = fast_atoull(&location);
    long priority = fast_atol(&location);
    long nice = fast_atol(&location);
    long nlwp = fast_atol(&location);
    /* skip (21) itrealvalue */
    if (!advance_to_next(&location)) { fprintf(stderr, "    stat: no 21\n"); return 0; }
    /* (22) starttime */
    unsigned long long starttime = fast_atoull(&location);
    if (!advance_to_next(&location)) { fprintf(stderr, "    stat: no 22\n"); return 0; }
    /* skip (23)-(38) */
    for (int i = 0; i < 16; i++) {
        if (!advance_to_next(&location)) { fprintf(stderr, "    stat: no 23-38 at %d\n", i); return 0; }
    }
    long processor = fast_atol(&location);
    fprintf(stderr,
            "    stat parse OK: ppid=%ld pgrp=%ld sess=%ld tty=%lu tpgid=%ld flags=%lu "
            "minflt=%llu minflt_c=%llu majflt=%llu majflt_c=%llu utime=%llu stime=%llu "
            "cutime=%llu cstime=%llu prio=%ld nice=%ld nthreads=%ld start=%llu cpu=%ld\n",
            ppid, pgrp, session, tty_nr, tpgid, flags, minflt, cminflt, majflt, cmajflt,
            utime, stime, cutime, cstime, priority, nice, nlwp, starttime, processor);
    return 1;
}

static void probe_per_pid(void) {
    fprintf(stderr, "=== Probe 2: per-pid openat/statm/stat/fstat (htop order) ===\n");

    int rawFd = open("/proc", O_RDONLY | O_DIRECTORY);
    if (rawFd >= 0) { getdents_loop(rawFd, 2); close(rawFd); }

    int procFd = open("/proc", O_RDONLY | O_DIRECTORY);
    if (procFd < 0) { fprintf(stderr, "  open /proc FAILED\n"); return; }

    DIR *dp = opendir("/proc");
    if (!dp) { fprintf(stderr, "  opendir /proc FAILED\n"); return; }

    int seen = 0, opened = 0, statm_ok = 0, fstat_ok = 0, stat_ok = 0, allseen = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        allseen++;
        if (allseen < 60)
            fprintf(stderr, "    [de %2d] '%s'\n", allseen, de->d_name);
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        if (!numeric(de->d_name)) continue;
        seen++;

        int pidFd = openat(procFd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (pidFd < 0) {
            fprintf(stderr, "  PID %s: openat(O_DIRECTORY|O_NOFOLLOW) FAILED: %s\n",
                    de->d_name, strerror(errno));
            continue;
        }
        opened++;

        struct stat sb;
        if (fstat(pidFd, &sb) == 0) {
            fstat_ok++;
            if (!S_ISDIR(sb.st_mode))
                fprintf(stderr, "  PID %s: fstat mode %#o not dir\n", de->d_name, sb.st_mode);
        } else {
            fprintf(stderr, "  PID %s: fstat FAILED: %s\n", de->d_name, strerror(errno));
        }

        char statmdata[128] = {0};
        ssize_t r = 0;
        int statmFd = openat(pidFd, "statm", O_RDONLY);
        if (statmFd >= 0) {
            r = read(statmFd, statmdata, sizeof(statmdata) - 1);
            close(statmFd);
        }
        long virt, res, share, trs, at, drs, dt;
        int n7 = sscanf(statmdata, "%ld %ld %ld %ld %ld %ld %ld",
                        &virt, &res, &share, &trs, &at, &drs, &dt);
        if (statmFd >= 0 && r >= 1 && n7 == 7) statm_ok++;
        else
            fprintf(stderr, "  PID %s: statm read=%zd sscanf=%d [%s]\n",
                    de->d_name, r, n7, statmdata);

        char sbuf[1024];
        int statFd = openat(pidFd, "stat", O_RDONLY);
        if (statFd >= 0) {
            r = read(statFd, sbuf, sizeof(sbuf) - 1);
            close(statFd);
            if (r > 0) {
                sbuf[r] = '\0';
                if (htop_parse_stat(sbuf, (unsigned long) atoi(de->d_name))) stat_ok++;
            } else {
                fprintf(stderr, "  PID %s: stat empty (read %zd)\n", de->d_name, r);
            }
        } else {
            fprintf(stderr, "  PID %s: open stat FAILED: %s\n", de->d_name, strerror(errno));
        }

        close(pidFd);
    }
    closedir(dp);
    close(procFd);
    fprintf(stderr, "  probe2 summary: seen=%d allseen=%d opened=%d fstat_ok=%d statm_ok=%d stat_ok=%d\n\n",
            seen, allseen, opened, fstat_ok, statm_ok, stat_ok);
}

static void probe_smallbuf_walk(void) {
    fprintf(stderr, "=== Probe 3: 64-byte getdents64 walk + openat per pid (htop test) ===\n");
    int fd = open("/proc", O_RDONLY | O_DIRECTORY);
    if (fd < 0) { fprintf(stderr, "  open /proc FAILED\n"); return; }
    char raw[64];
    int calls = 0, entries = 0, pids = 0;
    size_t seen[512]; int n = 0;
    for (;;) {
        ssize_t r = syscall(217, fd, raw, sizeof(raw));
        if (r < 0) { fprintf(stderr, "  call %d FAILED: %s\n", calls, strerror(errno)); break; }
        if (r == 0) { fprintf(stderr, "  call %d: EOF\n", calls); break; }
        calls++;
        fprintf(stderr, "  call %d: %zd bytes", calls - 1, r);
        ssize_t off = 0;
        while (off < r) {
            struct linux_dirent64 *d = (struct linux_dirent64 *) (raw + off);
            if (d->d_reclen == 0) break;
            fprintf(stderr, " %s", d->d_name);
            entries++;
            if (numeric(d->d_name)) {
                pids++;
                if (n < 512) seen[n++] = (size_t) atoi(d->d_name);
                int pfd = openat(fd, d->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
                if (pfd < 0) fprintf(stderr, "[open %s fail: %s]", d->d_name, strerror(errno));
                else close(pfd);
            }
            off += (int) d->d_reclen;
        }
        fprintf(stderr, "\n");
    }
    close(fd);
    int dups = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (seen[i] == seen[j]) dups++;
    fprintf(stderr, "  smallbuf: calls=%d entries=%d pids=%d unique=%d dup_hits=%d\n\n",
            calls, entries, pids, n, dups);
}

int main(void) {
    fprintf(stderr, "=== Stress: forking 140 sleepers to exceed 2048B dir listing ===\n");
    for (int i = 0; i < 140; i++) {
        pid_t p = fork();
        if (p == 0) { for (;;) sleep(1000); }
    }
    sleep(2);
    probe_getdents_stability();
    probe_getdents_stability();
    probe_getdents_stability();
    probe_smallbuf_walk();
    probe_per_pid();
    return 0;
}
