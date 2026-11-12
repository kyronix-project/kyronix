#include "test_harness.h"

static int parse_maps_line(const char *line, unsigned long *start, unsigned long *end, char perms[5],
                           unsigned long *offset, unsigned *maj, unsigned *min, unsigned long *ino,
                           const char **name) {
    int consumed = 0;
    if (sscanf(line, "%lx-%lx %4s %lx %x:%x %lu %n", start, end, perms, offset, maj, min, ino,
               &consumed) < 7)
        return 0;
    if (consumed <= 0) return 0;

    if (*end <= *start) return 0;
    if ((*start & 0xfff) || (*end & 0xfff)) return 0;

    if (perms[0] != 'r' && perms[0] != '-') return 0;
    if (perms[1] != 'w' && perms[1] != '-') return 0;
    if (perms[2] != 'x' && perms[2] != '-') return 0;
    if (perms[3] != 'p' && perms[3] != 's') return 0;

    const char *p = line + consumed;
    while (*p == ' ' || *p == '\t')
        p++;
    *name = p;
    return 1;
}

static int read_maps(char *buf, size_t bufsz) {
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return -1;
    size_t total = 0;
    for (;;) {
        if (total + 1 >= bufsz) break;
        ssize_t n = read(fd, buf + total, bufsz - 1 - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total += (size_t) n;
    }
    close(fd);
    buf[total] = '\0';
    return (int) total;
}

int test_maps_lines_parse(void) {
    char buf[65536];
    int n = read_maps(buf, sizeof(buf));
    ASSERT_GT(n, 0);

    int lines = 0;
    unsigned long prev_end = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        unsigned long start, end, offset, ino;
        unsigned maj, min;
        char perms[5];
        const char *name;
        ASSERT_TRUE(parse_maps_line(line, &start, &end, perms, &offset, &maj, &min, &ino, &name));
        ASSERT_GE(start, prev_end);
        prev_end = end;
        lines++;
    }
    ASSERT_GT(lines, 2);
    return 1;
}
REGISTER_TEST(maps_lines_parse, "Phase 12: proc maps");


int test_maps_no_placeholders(void) {
    char buf[65536];
    ASSERT_GT(read_maps(buf, sizeof(buf)), 0);
    ASSERT_NULL(strstr(buf, "xxxxxxxx"));
    ASSERT_NULL(strstr(buf, "yyyyyyyy"));
    return 1;
}
REGISTER_TEST(maps_no_placeholders, "Phase 12: proc maps");


int test_maps_exe_is_executable(void) {
    char buf[65536];
    ASSERT_GT(read_maps(buf, sizeof(buf)), 0);

    ASSERT_NOTNULL(strstr(buf, "r-xp"));

    char exe[PATH_MAX];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (k <= 0) {
        snprintf(exe, sizeof(exe), "testrunner");
        k = (ssize_t) strlen(exe);
    }
    exe[k] = '\0';

    const char *base = strrchr(exe, '/');
    base = base ? base + 1 : exe;
    ASSERT_TRUE(strstr(buf, exe) != NULL || strstr(buf, base) != NULL);
    return 1;
}
REGISTER_TEST(maps_exe_is_executable, "Phase 12: proc maps");


int test_maps_stack_present(void) {
    char buf[65536];
    ASSERT_GT(read_maps(buf, sizeof(buf)), 0);
    ASSERT_NOTNULL(strstr(buf, "[stack]"));
    ASSERT_NOTNULL(strstr(buf, "rw-p"));
    return 1;
}
REGISTER_TEST(maps_stack_present, "Phase 12: proc maps");


static int maps_line_for(const char *buf, unsigned long addr, unsigned long *start,
                         unsigned long *end, char perms[5], unsigned long *offset, unsigned long *ino,
                         const char **name) {
    char *copy = strdup(buf);
    if (!copy) return 0;
    int found = 0;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        unsigned long s, e, o, i;
        unsigned ma, mi;
        char pm[5];
        const char *nm;
        if (!parse_maps_line(line, &s, &e, pm, &o, &ma, &mi, &i, &nm)) continue;
        if (addr < s || addr >= e) continue;
        *start = s;
        *end = e;
        memcpy(perms, pm, 5);
        *offset = o;
        *ino = i;
        *name = nm;
        found = 1;
        break;
    }
    if (!found) free(copy);
    return found;
}

int test_maps_shows_new_mapping(void) {
    size_t len = 8 * 4096;
    void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(MAP_FAILED, m);
    memset(m, 0x5a, len);

    char buf[65536];
    ASSERT_GT(read_maps(buf, sizeof(buf)), 0);
    munmap(m, len);

    unsigned long s, e, o, ino;
    char perms[5];
    const char *name;
    ASSERT_TRUE(maps_line_for(buf, (unsigned long) m, &s, &e, perms, &o, &ino, &name));
    ASSERT_LE(s, (unsigned long) m);
    ASSERT_GE(e, (unsigned long) m + len);
    ASSERT_STREQ("rw-p", perms);
    return 1;
}
REGISTER_TEST(maps_shows_new_mapping, "Phase 12: proc maps");


int test_maps_tracks_mprotect(void) {
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(MAP_FAILED, m);
    memset(m, 1, 4096);
    ASSERT_EQ(0, mprotect(m, 4096, PROT_READ));

    char buf[65536];
    ASSERT_GT(read_maps(buf, sizeof(buf)), 0);

    unsigned long s, e, o, ino;
    char perms[5];
    const char *name;
    int found = maps_line_for(buf, (unsigned long) m, &s, &e, perms, &o, &ino, &name);
    munmap(m, 4096);
    ASSERT_TRUE(found);
    ASSERT_STREQ("r--p", perms);
    return 1;
}
REGISTER_TEST(maps_tracks_mprotect, "Phase 12: proc maps");

int test_maps_file_mapping_named(void) {
    const char *path = "/tmp/maps-file";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, ftruncate(fd, 16384));
    void *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
    ASSERT_NE(MAP_FAILED, m);
    memset(m, 0x11, 8192);

    char buf[65536];
    ASSERT_GT(read_maps(buf, sizeof(buf)), 0);

    unsigned long s, e, o, ino;
    char perms[5];
    const char *name;
    int found = maps_line_for(buf, (unsigned long) m, &s, &e, perms, &o, &ino, &name);
    if (found) {
        ASSERT_NOTNULL(strstr(name, "maps-file"));
        ASSERT_EQ(4096UL + ((unsigned long) m - s), o);
        ASSERT_EQ('s', perms[3]);
        ASSERT_GT(ino, 0UL);
    }
    munmap(m, 8192);
    close(fd);
    unlink(path);
    ASSERT_TRUE(found);
    return 1;
}
REGISTER_TEST(maps_file_mapping_named, "Phase 12: proc maps");

int test_maps_chunked_reads(void) {
    char whole[65536];
    int fd = open("/proc/self/maps", O_RDONLY);
    ASSERT_GE(fd, 0);
    ssize_t n = read(fd, whole, sizeof(whole) - 1);
    close(fd);
    ASSERT_GT(n, 64);
    whole[n] = '\0';

    fd = open("/proc/self/maps", O_RDONLY);
    ASSERT_GE(fd, 0);
    char piece[65536];
    ssize_t total = 0;
    while (total < n) {
        ssize_t r = read(fd, piece + total, 7);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return TEST_FAIL;
        }
        if (r == 0) break;
        total += r;
    }
    close(fd);
    piece[total] = '\0';
    ASSERT_EQ(n, total);
    ASSERT_STREQ(whole, piece);
    return 1;
}
REGISTER_TEST(maps_chunked_reads, "Phase 12: proc maps");
