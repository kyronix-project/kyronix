#include "test_harness.h"

#define WORK_ITER 50000000
static int busywork(void) {
    volatile long x = 0;
    for (long i = 1; i < WORK_ITER; i++) x += (i * i) / (i + 1);
    return (int) (x & 0xFF);
}

int test_smp_basic(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    ASSERT_GT(n, 1);
    printf("  [%ld CPUs online]\n", n);
    return 1;
}
REGISTER_TEST(smp_basic, "Phase 0: SMP");

int test_smp_parallel(void) {
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    ASSERT_GT(nproc, 1);

    /* sequential: do 2*nproc units of work on one CPU */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < nproc * 2; i++) busywork();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double seq_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    /* parallel: fork 2*nproc children, each does one unit */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < nproc * 2; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            busywork();
            _exit(0);
        }
        ASSERT_GT(pid, 0);
    }
    while (waitpid(-1, NULL, 0) > 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double par_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    /* Expect parallel to be faster (at least 10% speedup) */
    double speedup = seq_ms / par_ms;
    ASSERT_GT(speedup, 1.1);
    printf("  seq=%.0fms par=%.0fms speedup=%.1fx\n", seq_ms, par_ms, speedup);
    return 1;
}
REGISTER_TEST(smp_parallel, "Phase 0: SMP");

int test_smp_forkbomb(void) {
    const int N = 40;
    for (int i = 0; i < N; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            for (int j = 0; j < 5000; j++) sched_yield();
            _exit(0);
        }
        ASSERT_GT(pid, 0);
    }
    while (waitpid(-1, NULL, 0) > 0);
    return 1;
}
REGISTER_TEST(smp_forkbomb, "Phase 0: SMP");

int test_smp_concurrent_exec(void) {
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    for (int i = 0; i < nproc * 2; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/true", "true", NULL);
            _exit(1); /* should not reach */
        }
        ASSERT_GT(pid, 0);
    }
    while (waitpid(-1, NULL, 0) > 0);
    return 1;
}
REGISTER_TEST(smp_concurrent_exec, "Phase 0: SMP");

int test_smp_pingpong(void) {
    int p1[2], p2[2];
    ASSERT_EQ(pipe(p1), 0);
    ASSERT_EQ(pipe(p2), 0);

    pid_t child = fork();

    if (child == 0) {
        close(p1[1]);
        close(p2[0]);
        char buf = 0;
        for (int i = 0; i < 5000; i++) {
            ASSERT_EQ(read(p1[0], &buf, 1), 1);
            ASSERT_EQ(write(p2[1], &buf, 1), 1);
        }
        close(p1[0]);
        close(p2[1]);
        _exit(0);
    }

    ASSERT_GT(child, 0);
    close(p1[0]);
    close(p2[1]);
    char buf = 42;
    for (int i = 0; i < 5000; i++) {
        ASSERT_EQ(write(p1[1], &buf, 1), 1);
        ASSERT_EQ(read(p2[0], &buf, 1), 1);
    }
    close(p1[1]);
    close(p2[0]);
    int st;
    waitpid(child, &st, 0);
    ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    return 1;
}
REGISTER_TEST(smp_pingpong, "Phase 0: SMP");
