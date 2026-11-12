#include "test_harness.h"

#define NR_rseq 334
#define NR_close_range 436

#define RSEQ_FLAG_UNREGISTER (1 << 0)
#define RSEQ_SIG 0x53053053

typedef struct {
    uint32_t cpu_id_start;
    uint32_t cpu_id;
    uint64_t rseq_cs;
    uint32_t flags;
    uint32_t padding[3];
} rseq_t;

static int fd_is_open(int fd) {
    return fcntl(fd, F_GETFD) != -1;
}

int test_abi_rseq_is_not_close_range(void) {
    rseq_t r __attribute__((aligned(32)));
    memset(&r, 0, sizeof(r));

    long rc = syscall(NR_rseq, &r, sizeof(r), 0, RSEQ_SIG);
    ASSERT_EQ(0, rc);

    ASSERT_TRUE(fd_is_open(STDIN_FILENO));
    ASSERT_TRUE(fd_is_open(STDOUT_FILENO));
    ASSERT_TRUE(fd_is_open(STDERR_FILENO));
    return 1;
}
REGISTER_TEST(abi_rseq_is_not_close_range, "Phase 10: Security ABI");

int test_abi_rseq_zeroes_cs(void) {
    rseq_t r __attribute__((aligned(32)));
    memset(&r, 0, sizeof(r));
    r.rseq_cs = 0xdeadbeef;

    ASSERT_EQ(0, syscall(NR_rseq, &r, sizeof(r), 0, RSEQ_SIG));
    ASSERT_EQ(0, (long) r.rseq_cs);
    return 1;
}
REGISTER_TEST(abi_rseq_zeroes_cs, "Phase 10: Security ABI");

int test_abi_rseq_bad_len(void) {
    rseq_t r __attribute__((aligned(32)));
    memset(&r, 0, sizeof(r));

    errno = 0;
    long rc = syscall(NR_rseq, &r, sizeof(r) + 8, 0, RSEQ_SIG);
    ASSERT_EQ(-1, rc);
    ASSERT_EQ(EINVAL, errno);
    return 1;
}
REGISTER_TEST(abi_rseq_bad_len, "Phase 10: Security ABI");

int test_abi_close_range(void) {
    int fds[3];
    for (int i = 0; i < 3; i++) {
        fds[i] = open("/etc/fstab", O_RDONLY);
        ASSERT_GE(fds[i], 0);
    }
    ASSERT_EQ(0, syscall(NR_close_range, fds[0], fds[2], 0));
    ASSERT_FALSE(fd_is_open(fds[0]));
    ASSERT_FALSE(fd_is_open(fds[1]));
    ASSERT_FALSE(fd_is_open(fds[2]));

    ASSERT_TRUE(fd_is_open(STDOUT_FILENO));
    return 1;
}
REGISTER_TEST(abi_close_range, "Phase 10: Security ABI");

int test_abi_close_range_bounded(void) {
    int lo = open("/etc/fstab", O_RDONLY);
    ASSERT_GE(lo, 0);
    int hi = open("/etc/fstab", O_RDONLY);
    ASSERT_GE(hi, 0);
    ASSERT_GT(hi, lo);

    ASSERT_EQ(0, syscall(NR_close_range, lo, lo, 0));
    ASSERT_FALSE(fd_is_open(lo));
    ASSERT_TRUE(fd_is_open(hi));

    close(hi);
    return 1;
}
REGISTER_TEST(abi_close_range_bounded, "Phase 10: Security ABI");

int test_abi_close_range_bad_flags(void) {
    errno = 0;
    long rc = syscall(NR_close_range, 0, 1, 0x4000);
    ASSERT_EQ(-1, rc);
    ASSERT_EQ(EINVAL, errno);
    return 1;
}
REGISTER_TEST(abi_close_range_bad_flags, "Phase 10: Security ABI");
