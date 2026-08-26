#include "test_harness.h"

int test_time(void) {
    time_t t1 = time(NULL);
    ASSERT_GT(t1, 0);

    time_t t2 = time(NULL);
    ASSERT_GE(t2, t1); /* monotonic-ish */

    return 1;
}
REGISTER_TEST(time, "Phase 6: Timers & Time");

int test_gettimeofday(void) {
    struct timeval tv;
    ASSERT_EQ(0, gettimeofday(&tv, NULL));
    ASSERT_GT(tv.tv_sec, 0);

    struct timeval tv2;
    ASSERT_EQ(0, gettimeofday(&tv2, NULL));
    ASSERT_TRUE(tv2.tv_sec > tv.tv_sec || (tv2.tv_sec == tv.tv_sec && tv2.tv_usec >= tv.tv_usec));
    return 1;
}
REGISTER_TEST(gettimeofday, "Phase 6: Timers & Time");

int test_clock_gettime(void) {
    struct timespec ts;

    ASSERT_EQ(0, clock_gettime(CLOCK_REALTIME, &ts));
    ASSERT_GT(ts.tv_sec, 0);

    ASSERT_EQ(0, clock_gettime(CLOCK_MONOTONIC, &ts));
    ASSERT_GT(ts.tv_sec, 0);

    int ret = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    if (ret < 0 && (errno == ENOSYS || errno == EINVAL)) return 1;
    ASSERT_EQ(0, ret);

    ret = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    if (ret < 0 && (errno == ENOSYS || errno == EINVAL)) return 1;
    ASSERT_EQ(0, ret);

    return 1;
}
REGISTER_TEST(clock_gettime, "Phase 6: Timers & Time");

int test_clock_settime(void) {
    struct timespec original, desired, actual;
    ASSERT_EQ(0, clock_gettime(CLOCK_REALTIME, &original));

    desired.tv_sec = original.tv_sec + 2;
    desired.tv_nsec = 123000000;
    ASSERT_EQ(0, clock_settime(CLOCK_REALTIME, &desired));
    ASSERT_EQ(0, clock_gettime(CLOCK_REALTIME, &actual));
    ASSERT_TRUE(actual.tv_sec > desired.tv_sec ||
                (actual.tv_sec == desired.tv_sec && actual.tv_nsec >= desired.tv_nsec));
    ASSERT_TRUE(actual.tv_sec <= desired.tv_sec + 1);

    struct timespec invalid = desired;
    invalid.tv_nsec = 1000000000L;
    errno = 0;
    ASSERT_EQ(-1, clock_settime(CLOCK_REALTIME, &invalid));
    ASSERT_EQ(EINVAL, errno);

    errno = 0;
    ASSERT_EQ(-1, clock_settime(CLOCK_MONOTONIC, &desired));
    ASSERT_EQ(EINVAL, errno);

    ASSERT_EQ(0, clock_settime(CLOCK_REALTIME, &original));
    return 1;
}
REGISTER_TEST(clock_settime, "Phase 6: Timers & Time");

int test_clock_getres(void) {
    struct timespec ts;

    ASSERT_EQ(0, clock_getres(CLOCK_REALTIME, &ts));
    /* resolution lives in tv_nsec and must be non-zero but sub-second */
    ASSERT_GT(ts.tv_nsec, 0);
    ASSERT_LT(ts.tv_nsec, 1000000000L);

    ASSERT_EQ(0, clock_getres(CLOCK_MONOTONIC, &ts));
    ASSERT_LT(ts.tv_nsec, 1000000000L);

    return 1;
}
REGISTER_TEST(clock_getres, "Phase 6: Timers & Time");

int test_clock_nanosleep(void) {
    struct timespec req, now;

    /* relative sleep ~20ms */
    req.tv_sec = 0;
    req.tv_nsec = 20000000;
    int ret = clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);
    if (ret == ENOSYS) return 1;
    ASSERT_EQ(0, ret);

    /* absolute sleep until now + 20ms (TIMER_ABSTIME) */
    ASSERT_EQ(0, clock_gettime(CLOCK_MONOTONIC, &now));
    req = now;
    req.tv_nsec += 20000000;
    if (req.tv_nsec >= 1000000000L) {
        req.tv_sec++;
        req.tv_nsec -= 1000000000L;
    }
    ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, NULL);
    ASSERT_EQ(0, ret);
    return 1;
}
REGISTER_TEST(clock_nanosleep, "Phase 6: Timers & Time");

int test_nanosleep(void) {
    struct timespec req, rem;
    req.tv_sec = 0;
    req.tv_nsec = 10000000; /* 10ms */
    int ret = nanosleep(&req, &rem);
    if (ret < 0 && errno == ENOSYS) return 1;
    ASSERT_EQ(0, ret);

    return 1;
}
REGISTER_TEST(nanosleep, "Phase 6: Timers & Time");

int test_getitimer_setitimer(void) {
    struct itimerval old, curr;

    /* ITIMER_REAL */
    memset(&old, 0, sizeof(old));
    old.it_value.tv_sec = 1;
    int ret = setitimer(ITIMER_REAL, &old, NULL);
    if (ret < 0 && (errno == ENOSYS || errno == EINVAL || errno == ENOTSUP)) return 1;
    ASSERT_EQ(0, ret);

    ret = getitimer(ITIMER_REAL, &curr);
    if (ret < 0 && (errno == ENOSYS || errno == EINVAL)) return 1;
    ASSERT_EQ(0, ret);

    /* Cancel */
    memset(&old, 0, sizeof(old));
    ASSERT_EQ(0, setitimer(ITIMER_REAL, &old, NULL));

    /* ITIMER_VIRTUAL */
    memset(&old, 0, sizeof(old));
    ret = setitimer(ITIMER_VIRTUAL, &old, NULL);
    if (ret < 0 && (errno == ENOSYS || errno == EINVAL || errno == ENOTSUP)) return 1;
    ASSERT_EQ(0, ret);

    /* ITIMER_PROF */
    memset(&old, 0, sizeof(old));
    ret = setitimer(ITIMER_PROF, &old, NULL);
    if (ret < 0 && (errno == ENOSYS || errno == EINVAL || errno == ENOTSUP)) return 1;
    ASSERT_EQ(0, ret);

    return 1;
}
REGISTER_TEST(getitimer_setitimer, "Phase 6: Timers & Time");

int test_alarm_basic(void) {
    unsigned prev;
    prev = alarm(1);
    ASSERT_EQ(0, prev);

    prev = alarm(0);
    if (prev == 0 && errno == ENOSYS) return 1;

    prev = alarm(3);
    ASSERT_EQ(0, prev);
    prev = alarm(5);
    alarm(0);

    return 1;
}
REGISTER_TEST(alarm_basic, "Phase 6: Timers & Time");

int test_posix_timers(void) {
    timer_t timerid;
    struct sigevent sev;
    struct itimerspec its;

    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGALRM;

    int ret = timer_create(CLOCK_REALTIME, &sev, &timerid);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP || errno == EINVAL)) return 1;
    ASSERT_EQ(0, ret);

    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = 1;
    ret = timer_settime(timerid, 0, &its, NULL);
    if (ret < 0 && (errno == ENOSYS || errno == EINVAL)) return 1;
    ASSERT_EQ(0, ret);

    ret = timer_gettime(timerid, &its);
    if (ret < 0 && (errno == ENOSYS || errno == EINVAL)) return 1;
    ASSERT_EQ(0, ret);

    int overrun = timer_getoverrun(timerid);
    if (overrun < 0 && (errno == ENOSYS || errno == EINVAL)) return 1;
    ASSERT_GE(overrun, 0);

    ret = timer_delete(timerid);
    if (ret < 0 && (errno == ENOSYS || errno == EINVAL)) return 1;
    ASSERT_EQ(0, ret);

    return 1;
}
REGISTER_TEST(posix_timers, "Phase 6: Timers & Time");

int test_times(void) {
    struct tms buf;
    clock_t t = times(&buf);
    if (t < 0 && errno == ENOSYS) return 1;
    ASSERT_GT(t, 0);

    /* tms_utime and tms_stime should be >= 0 */
    ASSERT_GE(buf.tms_utime, 0);
    ASSERT_GE(buf.tms_stime, 0);
    ASSERT_GE(buf.tms_cutime, 0);
    ASSERT_GE(buf.tms_cstime, 0);

    return 1;
}
REGISTER_TEST(times, "Phase 6: Timers & Time");

/*
 * libwayland arms every event-loop timer with TFD_TIMER_ABSTIME, so an
 * absolute deadline must not be mistaken for a relative delay.
 */
int test_timerfd_abstime(void) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tfd < 0 && (errno == ENOSYS || errno == EINVAL)) return 1;
    ASSERT_GE(tfd, 0);

    struct timespec now;
    ASSERT_EQ(0, clock_gettime(CLOCK_MONOTONIC, &now));

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value = now;
    its.it_value.tv_nsec += 60 * 1000 * 1000; /* +60 ms */
    if (its.it_value.tv_nsec >= 1000000000L) {
        its.it_value.tv_nsec -= 1000000000L;
        its.it_value.tv_sec++;
    }
    ASSERT_EQ(0, timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL));

    /* not expired yet */
    uint64_t ticks = 0;
    ASSERT_EQ((ssize_t) -1, read(tfd, &ticks, sizeof(ticks)));
    ASSERT(errno == EAGAIN);

    int ep = epoll_create1(0);
    ASSERT_GE(ep, 0);
    struct epoll_event ev = { .events = EPOLLIN, .data = { .fd = tfd } };
    ASSERT_EQ(0, epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev));

    struct epoll_event out[1];
    /* a relative-vs-absolute mixup would push this far into the future */
    ASSERT_EQ(1, epoll_wait(ep, out, 1, 1000));
    ASSERT_EQ((ssize_t) sizeof(ticks), read(tfd, &ticks, sizeof(ticks)));
    ASSERT_GE(ticks, (uint64_t) 1);

    close(ep);
    close(tfd);
    return 1;
}
REGISTER_TEST(timerfd_abstime, "Phase 6: Timers & Time");
