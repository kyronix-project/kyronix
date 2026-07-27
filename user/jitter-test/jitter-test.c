#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SYS_ANTI_TOCTOU 511
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#define ANTI_TOCTOU_CTL_QUERY 0u
#define ANTI_TOCTOU_CTL_ENABLE 1u
#define ANTI_TOCTOU_CTL_DISABLE 2u
#define ANTI_TOCTOU_CTL_RESET 3u
#define ANTI_TOCTOU_CTL_STATS 4u

#define VFS_ITERATIONS 64
#define MEMORY_ITERATIONS 128

typedef struct {
    uint32_t enabled;
    uint32_t min_delay_us;
    uint32_t max_delay_us;
    uint32_t queue_capacity;
    uint64_t vfs_observations;
    uint64_t memory_observations;
    uint64_t vfs_detections;
    uint64_t memory_detections;
    uint64_t delays_queued;
    uint64_t delays_applied;
    uint64_t queue_drops;
    uint64_t total_delay_us;
    uint32_t last_delay_us;
    uint32_t reserved;
} anti_toctou_stats_t;

static const char *g_path = "/etc/os-release";
static atomic_int g_ready;
static atomic_int g_start;
static atomic_int g_vfs_done;
static atomic_int g_turn;
static uint32_t g_futex_word;
static atomic_int g_failures;
static int g_read_fd = -1;

static inline void relax(void) {
    __asm__ volatile("pause" ::: "memory");
}

static void fail(const char *what) {
    fprintf(stderr, "[FAIL] %s: %s\n", what, strerror(errno));
    atomic_fetch_add_explicit(&g_failures, 1, memory_order_relaxed);
}

static void *racer(void *opaque) {
    uintptr_t id = (uintptr_t) opaque;
    atomic_fetch_add_explicit(&g_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&g_start, memory_order_acquire))
        relax();

    for (int i = 0; i < VFS_ITERATIONS; i++) {
        while (atomic_load_explicit(&g_turn, memory_order_acquire) !=
               (int) id)
            relax();
        if (id == 0) {
            if (access(g_path, F_OK) != 0) fail("access");
        } else {
            char byte;
            if (read(g_read_fd, &byte, 1) < 0) fail("read");
        }
        atomic_store_explicit(&g_turn, (int) (id ^ 1u),
                              memory_order_release);
    }

    atomic_fetch_add_explicit(&g_vfs_done, 1, memory_order_release);
    while (atomic_load_explicit(&g_vfs_done, memory_order_acquire) != 2)
        relax();

    for (int i = 0; i < MEMORY_ITERATIONS; i++) {
        while (atomic_load_explicit(&g_turn, memory_order_acquire) !=
               (int) id)
            relax();
        long r = syscall(SYS_futex, &g_futex_word,
                         FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL, NULL, 0);
        if (r < 0) fail("futex wake");
        atomic_store_explicit(&g_turn, (int) (id ^ 1u),
                              memory_order_release);
    }
    return NULL;
}

static int check(int condition, const char *message) {
    printf("[%s] %s\n", condition ? " OK " : "FAIL", message);
    return condition ? 0 : 1;
}

int main(void) {
    int failures = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Anti-TOCTOU scheduler jitter self-test\n");

    long disabled =
        syscall(SYS_ANTI_TOCTOU, ANTI_TOCTOU_CTL_DISABLE, NULL);
    failures += check(disabled == 0, "jitter paused during setup");

    failures += check(access(g_path, R_OK) == 0, "probe VFS node available");
    g_read_fd = open(g_path, O_RDONLY);
    if (g_read_fd < 0) {
        perror("open probe VFS node");
        return 1;
    }

    pthread_t threads[2];
    for (uintptr_t i = 0; i < 2; i++) {
        int rc = pthread_create(&threads[i], NULL, racer, (void *) i);
        if (rc != 0) {
            errno = rc;
            perror("pthread_create");
            return 1;
        }
    }
    while (atomic_load_explicit(&g_ready, memory_order_acquire) != 2)
        sched_yield();
    failures += check(1, "racer threads ready");

    if (syscall(SYS_ANTI_TOCTOU, ANTI_TOCTOU_CTL_RESET, NULL) != 0) {
        perror("reset jitter stats");
        return 1;
    }
    long enabled =
        syscall(SYS_ANTI_TOCTOU, ANTI_TOCTOU_CTL_ENABLE, NULL);
    failures += check(enabled == 1, "jitter enabled");
    atomic_store_explicit(&g_start, 1, memory_order_release);

    for (int i = 0; i < 2; i++) {
        int rc = pthread_join(threads[i], NULL);
        if (rc != 0) {
            errno = rc;
            perror("pthread_join");
            return 1;
        }
    }

    anti_toctou_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (syscall(SYS_ANTI_TOCTOU, ANTI_TOCTOU_CTL_STATS, &stats) != 0) {
        perror("read jitter stats");
        return 1;
    }

    failures += atomic_load_explicit(&g_failures, memory_order_relaxed);
    failures += check(stats.enabled == 1, "controller reports enabled");
    failures += check(stats.vfs_observations >= 100,
                      "shared VFS node observed");
    failures += check(stats.vfs_detections > 0,
                      "VFS check/use race detected");
    failures += check(stats.memory_observations >= 100,
                      "shared memory region observed");
    failures += check(stats.memory_detections > 0,
                      "memory race detected");
    failures += check(stats.delays_queued > 0 &&
                          stats.delays_applied == stats.delays_queued,
                      "all jitter wake-ups applied");
    failures += check(stats.last_delay_us >= stats.min_delay_us &&
                          stats.last_delay_us <= stats.max_delay_us,
                      "delay stayed inside 25..250 us bound");

    close(g_read_fd);
    printf("stats: vfs=%llu/%llu memory=%llu/%llu delays=%llu drops=%llu\n",
           (unsigned long long) stats.vfs_detections,
           (unsigned long long) stats.vfs_observations,
           (unsigned long long) stats.memory_detections,
           (unsigned long long) stats.memory_observations,
           (unsigned long long) stats.delays_applied,
           (unsigned long long) stats.queue_drops);
    printf("Anti-TOCTOU self-test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
