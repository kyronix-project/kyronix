#include "common.h"
#include <signal.h>
#include <sys/wait.h>

static int g_nworkers = 0;
static int g_duration = 10;

static void cpu_burn(void) {
    volatile unsigned long acc = 1;
    for (unsigned long i = 2; i < 100000UL; i++) {
        acc = (acc * i + i) ^ ((acc >> 3) + i);
        acc += acc >> 5;
        acc ^= acc << 3;
    }
}

static void worker(int write_fd, int duration) {
    unsigned long long ops = 0;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        cpu_burn();
        ops++;

        if ((ops & 0xFF) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - start.tv_sec >= (time_t) duration) break;
        }
    }

    write(write_fd, &ops, sizeof(ops));
    _exit(0);
}

static double elapsed_sec(struct timespec *t0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) + (now.tv_nsec - t0->tv_nsec) / 1e9;
}

static void usage(void) {
    fprintf(stderr, "Usage: stress [-c workers] [-t seconds]\n"
                    "  -c N   number of worker processes (default: all CPUs)\n"
                    "  -t N   duration in seconds (default: 10)\n"
                    "  -h     show this help\n");
    exit(1);
}

int main(int argc, char **argv) {
    kx_prog = "stress";
    int ncores = (int) sysconf(_SC_NPROCESSORS_ONLN);
    if (ncores < 1) ncores = 1;
    g_nworkers = ncores;

    int opt;
    while ((opt = getopt(argc, argv, "c:t:h")) != -1) {
        switch (opt) {
        case 'c':
            g_nworkers = atoi(optarg);
            if (g_nworkers < 1) kx_die("need at least 1 worker");
            break;
        case 't':
            g_duration = atoi(optarg);
            if (g_duration < 1) kx_die("duration must be >= 1");
            break;
        case 'h':
        default:
            usage();
        }
    }

    printf("stress: %d workers x %d seconds on %d CPU(s)\n\n", g_nworkers, g_duration, ncores);

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pid_t *pids = malloc((size_t) g_nworkers * sizeof(pid_t));
    int (*pipes)[2] = malloc((size_t) g_nworkers * sizeof(int[2]));
    if (!pids || !pipes) kx_die("malloc");

    int spawned = 0;
    for (int i = 0; i < g_nworkers; i++) {
        if (pipe(pipes[i]) < 0) break;

        pid_t pid = fork();
        if (pid < 0) {
            close(pipes[i][0]);
            close(pipes[i][1]);
            break;
        }
        if (pid == 0) {
            free(pids);
            for (int j = 0; j <= i; j++) close(pipes[j][0]);
            for (int j = 0; j < i; j++) close(pipes[j][1]);
            worker(pipes[i][1], g_duration);
            _exit(0);
        }
        close(pipes[i][1]);
        pids[i] = pid;
        spawned++;
    }

    printf("  ELAPSED\n");
    printf("  -------\n");

    while (elapsed_sec(&t0) < (double) g_duration) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
        printf("\r  %5.1fs  ", elapsed_sec(&t0));
        fflush(stdout);
    }

    printf("\n\nstress: collecting results...\n");

    unsigned long long total_ops = 0;
    int waited = 0;
    while (waited < spawned) {
        int status;
        pid_t w = waitpid(-1, &status, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < spawned; i++) {
            if (pids[i] == w) {
                unsigned long long ops = 0;
                ssize_t n = read(pipes[i][0], &ops, sizeof(ops));
                if (n == (ssize_t) sizeof(ops)) total_ops += ops;
                close(pipes[i][0]);
                break;
            }
        }
        waited++;
    }

    double total = elapsed_sec(&t0);

    printf("\n  ELAPSED   WORKERS   OPS\n");
    printf("  -------   -------   ---\n");
    printf("  %5.1fs     %3d    %llu\n\n", total, spawned, total_ops);
    printf("stress: %.1f sec, %d workers, %llu ops, %.0f ops/sec\n", total, spawned, total_ops,
           total > 0 ? (double) total_ops / total : 0);

    free(pids);
    free(pipes);
    return 0;
}
