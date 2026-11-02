#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef KYRONIX_GUEST
#include <sys/reboot.h>
#define DEFAULT_CASES FUZZ_CASES
#define DEFAULT_SEED FUZZ_SEED
#define DEFAULT_ONLY FUZZ_ONLY
#else
#define DEFAULT_CASES 140
#define DEFAULT_SEED 1
#define DEFAULT_ONLY -1
#endif

static uint32_t rng;
static char why[160];

static uint32_t next_random(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

#define CHECK(expr) do { \
    if (!(expr)) { \
        snprintf(why, sizeof(why), "line=%d check=%s", __LINE__, #expr); \
        goto done; \
    } \
} while (0)

static const char *names[] = {
    "create_exclusive", "read_write", "positional_io", "shared_offset",
    "truncate", "pipe", "bad_descriptor"
};

static int run_case(unsigned index) {
    char path[80];
    unsigned kind = index % (sizeof(names) / sizeof(names[0]));
    unsigned len = 2 + next_random() % 127;
    unsigned off = next_random() % 64;
    unsigned char data[192], got[192];
    int fd = -1, other = -1, p[2] = {-1, -1};
    int ok = 0;
    struct stat st;

    snprintf(path, sizeof(path), "/tmp/posix-fuzz-%u", index);
    unlink(path);
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = (unsigned char) next_random();
    memset(got, 0xa5, sizeof(got));
    why[0] = '\0';

    switch (kind) {
    case 0:
        fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
        CHECK(fd >= 0);
        errno = 0;
        other = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
        CHECK(other == -1 && errno == EEXIST);
        CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
        break;
    case 1:
        fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
        CHECK(fd >= 0);
        CHECK(write(fd, data, len) == (ssize_t) len);
        CHECK(lseek(fd, 0, SEEK_CUR) == (off_t) len);
        CHECK(lseek(fd, 0, SEEK_SET) == 0);
        CHECK(read(fd, got, len) == (ssize_t) len);
        CHECK(memcmp(got, data, len) == 0);
        CHECK(read(fd, got, 1) == 0);
        break;
    case 2:
        fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
        CHECK(fd >= 0);
        CHECK(pwrite(fd, data, len, (off_t) off) == (ssize_t) len);
        CHECK(lseek(fd, 0, SEEK_CUR) == 0);
        CHECK(pread(fd, got, len, (off_t) off) == (ssize_t) len);
        CHECK(memcmp(got, data, len) == 0);
        CHECK(lseek(fd, 0, SEEK_CUR) == 0);
        CHECK(fstat(fd, &st) == 0 && st.st_size == (off_t) (off + len));
        break;
    case 3:
        fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
        CHECK(fd >= 0);
        CHECK(write(fd, data, len) == (ssize_t) len);
        CHECK(lseek(fd, 0, SEEK_SET) == 0);
        other = dup(fd);
        CHECK(other >= 0 && other != fd);
        CHECK(read(other, got, 1) == 1 && got[0] == data[0]);
        CHECK(lseek(fd, 0, SEEK_CUR) == 1);
        CHECK(read(fd, got, 1) == 1 && got[0] == data[1 % len]);
        CHECK(fcntl(fd, F_SETFL, O_APPEND) == 0);
        CHECK((fcntl(other, F_GETFL) & O_APPEND) != 0);
        CHECK(fcntl(other, F_SETFD, FD_CLOEXEC) == 0);
        CHECK((fcntl(other, F_GETFD) & FD_CLOEXEC) != 0);
        CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0);
        CHECK(close(fd) == 0);
        fd = -1;
        CHECK(lseek(other, 0, SEEK_CUR) == 2);
        break;
    case 4:
        fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
        CHECK(fd >= 0);
        CHECK(write(fd, data, len) == (ssize_t) len);
        CHECK(ftruncate(fd, (off_t) (len / 2)) == 0);
        CHECK(fstat(fd, &st) == 0 && st.st_size == (off_t) (len / 2));
        CHECK(ftruncate(fd, (off_t) (len + off)) == 0);
        CHECK(fstat(fd, &st) == 0 && st.st_size == (off_t) (len + off));
        CHECK(pread(fd, got, len / 2, 0) == (ssize_t) (len / 2));
        CHECK(memcmp(got, data, len / 2) == 0);
        memset(got, 0xa5, sizeof(got));
        CHECK(pread(fd, got, len + off - len / 2, len / 2) ==
              (ssize_t) (len + off - len / 2));
        for (unsigned i = 0; i < len + off - len / 2; i++) CHECK(got[i] == 0);
        break;
    case 5:
        CHECK(pipe(p) == 0);
        CHECK(write(p[1], data, len) == (ssize_t) len);
        CHECK(read(p[0], got, len) == (ssize_t) len);
        CHECK(memcmp(got, data, len) == 0);
        CHECK(close(p[1]) == 0);
        p[1] = -1;
        CHECK(read(p[0], got, 1) == 0);
        break;
    case 6:
        errno = 0;
        CHECK(read(-1, got, 1) == -1 && errno == EBADF);
        errno = 0;
        CHECK(write(-1, data, 1) == -1 && errno == EBADF);
        fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        CHECK(fd >= 0);
        errno = 0;
        CHECK(read(fd, got, 1) == -1 && errno == EBADF);
        break;
    }
    ok = 1;
done:
    if (fd >= 0) close(fd);
    if (other >= 0) close(other);
    if (p[0] >= 0) close(p[0]);
    if (p[1] >= 0) close(p[1]);
    unlink(path);
    return ok;
}

int main(int argc, char **argv) {
    unsigned seed = DEFAULT_SEED, cases = DEFAULT_CASES, only = (unsigned) DEFAULT_ONLY;
    int single = DEFAULT_ONLY >= 0, failures = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && ++i < argc) seed = (unsigned) strtoul(argv[i], NULL, 0);
        else if (strcmp(argv[i], "--cases") == 0 && ++i < argc) cases = (unsigned) strtoul(argv[i], NULL, 0);
        else if (strcmp(argv[i], "--case") == 0 && ++i < argc) {
            only = (unsigned) strtoul(argv[i], NULL, 0);
            single = 1;
        } else {
            fprintf(stderr, "usage: %s [--seed N] [--cases N] [--case N]\n", argv[0]);
            return 2;
        }
    }
    if (cases == 0 || cases > 100000 || (single && only >= cases)) return 2;
#ifdef KYRONIX_GUEST
    int tty = open("/dev/tty", O_RDWR);
    if (tty >= 0) {
        dup2(tty, STDOUT_FILENO);
        dup2(tty, STDERR_FILENO);
        if (tty > STDERR_FILENO) close(tty);
    }
#endif
    printf("POSIX_BEGIN seed=%u cases=%u\n", seed, single ? 1 : cases);
    fflush(stdout);
    for (unsigned i = 0; i < cases; i++) {
        if (single && i != only) continue;
        rng = seed ^ (0x9e3779b9u * (i + 1));
        if (rng == 0) rng = 1;
        int ok = run_case(i);
        printf("POSIX_CASE seed=%u index=%u api=%s status=%s%s%s\n",
               seed, i, names[i % 7], ok ? "pass" : "fail",
               ok ? "" : " ", ok ? "" : why);
        fflush(stdout);
        if (!ok) failures++;
    }
    printf("POSIX_END executed=%u failed=%d\n", single ? 1 : cases, failures);
    fflush(stdout);
#ifdef KYRONIX_GUEST
    reboot(0x01234567);
#endif
    return failures ? 1 : 0;
}
