#include "common.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#define NTP_PACKET_SIZE 48
#define NTP_UNIX_DELTA 2208988800ULL
#define NTP_ERA_SECONDS 4294967296LL
#define MIN_UNIX_TIME 946684800LL
#define MAX_UNIX_TIME 4133980800LL
#define QUERY_TIMEOUT_MS 5000
#define MAX_ROOT_DISPERSION (16U << 16)
#define MAX_DELAY_US 10000000LL

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static void put_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t) (value >> 24);
    p[1] = (uint8_t) (value >> 16);
    p[2] = (uint8_t) (value >> 8);
    p[3] = (uint8_t) value;
}

static int64_t timespec_us(const struct timespec *ts) {
    return (int64_t) ts->tv_sec * 1000000LL + ts->tv_nsec / 1000;
}

static void put_ntp_timestamp(uint8_t *out, const struct timespec *ts) {
    uint64_t seconds = (uint64_t) ts->tv_sec + NTP_UNIX_DELTA;
    uint64_t fraction = ((uint64_t) ts->tv_nsec << 32) / 1000000000ULL;
    put_be32(out, (uint32_t) seconds);
    put_be32(out + 4, (uint32_t) fraction);
}

static bool ntp_timestamp_us(const uint8_t *stamp, int64_t reference_sec, int64_t *result) {
    uint32_t raw_seconds = get_be32(stamp);
    uint32_t raw_fraction = get_be32(stamp + 4);
    int64_t best = 0;
    int64_t best_distance = INT64_MAX;

    for (int era = 0; era <= 1; era++) {
        int64_t seconds =
            (int64_t) raw_seconds + (int64_t) era * NTP_ERA_SECONDS - (int64_t) NTP_UNIX_DELTA;
        if (seconds < MIN_UNIX_TIME || seconds >= MAX_UNIX_TIME) continue;
        int64_t distance = seconds > reference_sec ? seconds - reference_sec
                                                   : reference_sec - seconds;
        if (distance < best_distance) {
            best = seconds;
            best_distance = distance;
        }
    }
    if (best_distance == INT64_MAX) return false;

    uint64_t fraction_us = ((uint64_t) raw_fraction * 1000000ULL) >> 32;
    *result = best * 1000000LL + (int64_t) fraction_us;
    return true;
}

static int query_address(const struct addrinfo *ai, int64_t *offset_us, int64_t *delay_us,
                         char *error, size_t error_size) {
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
        snprintf(error, error_size, "socket: %s", strerror(errno));
        return -1;
    }
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        snprintf(error, error_size, "connect: %s", strerror(errno));
        close(fd);
        return -1;
    }

    uint8_t request[NTP_PACKET_SIZE] = { 0 };
    request[0] = (uint8_t) ((4U << 3) | 3U);
    struct timespec t1;
    if (clock_gettime(CLOCK_REALTIME, &t1) < 0) {
        snprintf(error, error_size, "clock_gettime: %s", strerror(errno));
        close(fd);
        return -1;
    }
    put_ntp_timestamp(request + 40, &t1);

    if (send(fd, request, sizeof(request), 0) != (ssize_t) sizeof(request)) {
        snprintf(error, error_size, "send: %s", strerror(errno));
        close(fd);
        return -1;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready = poll(&pfd, 1, QUERY_TIMEOUT_MS);
    if (ready <= 0 || !(pfd.revents & POLLIN)) {
        snprintf(error, error_size, "%s", ready == 0 ? "request timed out" : "poll failed");
        close(fd);
        return -1;
    }

    uint8_t response[512];
    struct sockaddr_in source;
    socklen_t source_len = sizeof(source);
    ssize_t received = recvfrom(fd, response, sizeof(response), MSG_DONTWAIT,
                                (struct sockaddr *) &source, &source_len);
    struct timespec t4;
    int clock_error = clock_gettime(CLOCK_REALTIME, &t4);
    close(fd);

    if (received < NTP_PACKET_SIZE) {
        snprintf(error, error_size, "short or missing response");
        return -1;
    }
    if (clock_error < 0) {
        snprintf(error, error_size, "clock_gettime: %s", strerror(errno));
        return -1;
    }

    const struct sockaddr_in *peer = (const struct sockaddr_in *) ai->ai_addr;
    if (source_len < sizeof(source) || source.sin_family != AF_INET ||
        source.sin_addr.s_addr != peer->sin_addr.s_addr || source.sin_port != peer->sin_port) {
        snprintf(error, error_size, "response source mismatch");
        return -1;
    }

    unsigned leap = response[0] >> 6;
    unsigned version = (response[0] >> 3) & 7U;
    unsigned mode = response[0] & 7U;
    unsigned stratum = response[1];
    if (leap == 3 || version < 3 || version > 4 || mode != 4 ||
        stratum == 0 || stratum > 15) {
        snprintf(error, error_size, "invalid NTP status");
        return -1;
    }
    if (get_be32(response + 8) > MAX_ROOT_DISPERSION) {
        snprintf(error, error_size, "excessive root dispersion");
        return -1;
    }
    if (memcmp(response + 24, request + 40, 8) != 0) {
        snprintf(error, error_size, "originate timestamp mismatch");
        return -1;
    }

    int64_t t1_us = timespec_us(&t1);
    int64_t t4_us = timespec_us(&t4);
    int64_t t2_us, t3_us;
    if (!ntp_timestamp_us(response + 32, t4.tv_sec, &t2_us) ||
        !ntp_timestamp_us(response + 40, t4.tv_sec, &t3_us) ||
        t3_us < t2_us || t3_us - t2_us > MAX_DELAY_US) {
        snprintf(error, error_size, "invalid server timestamps");
        return -1;
    }

    int64_t delay = (t4_us - t1_us) - (t3_us - t2_us);
    if (delay < -100000LL || delay > MAX_DELAY_US) {
        snprintf(error, error_size, "implausible network delay");
        return -1;
    }

    *offset_us = ((t2_us - t1_us) + (t3_us - t4_us)) / 2;
    *delay_us = delay < 0 ? 0 : delay;
    return 0;
}

static int sync_server(const char *server, int64_t *applied_offset, int64_t *delay_us,
                       char *error, size_t error_size) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICSERV;

    struct addrinfo *addresses = NULL;
    int gai_error = getaddrinfo(server, "123", &hints, &addresses);
    if (gai_error) {
        snprintf(error, error_size, "DNS: %s", gai_strerror(gai_error));
        return -1;
    }

    int result = -1;
    int64_t offset = 0;
    int64_t delay = 0;
    for (struct addrinfo *ai = addresses; ai; ai = ai->ai_next) {
        if (query_address(ai, &offset, &delay, error, error_size) == 0) {
            result = 0;
            break;
        }
    }
    freeaddrinfo(addresses);
    if (result < 0) return -1;

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
        snprintf(error, error_size, "clock_gettime: %s", strerror(errno));
        return -1;
    }
    int64_t target_us = timespec_us(&now) + offset;
    if (target_us < MIN_UNIX_TIME * 1000000LL ||
        target_us >= MAX_UNIX_TIME * 1000000LL) {
        snprintf(error, error_size, "server time is outside the accepted range");
        return -1;
    }

    struct timespec target = {
        .tv_sec = (time_t) (target_us / 1000000LL),
        .tv_nsec = (long) ((target_us % 1000000LL) * 1000LL),
    };
    if (clock_settime(CLOCK_REALTIME, &target) < 0) {
        snprintf(error, error_size, "clock_settime: %s", strerror(errno));
        return -1;
    }

    *applied_offset = offset;
    *delay_us = delay;
    return 0;
}

static unsigned parse_interval(const char *value) {
    char *end = NULL;
    errno = 0;
    unsigned long interval = strtoul(value, &end, 10);
    if (errno || !*value || *end || interval == 0 || interval > 86400)
        kx_die("invalid interval");
    return (unsigned) interval;
}

int main(int argc, char **argv) {
    kx_prog = "ntpd";
    bool once = false;
    bool quiet = false;
    unsigned interval = 3600;
    const char *servers[16];
    size_t server_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) {
            once = true;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            interval = parse_interval(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            puts("usage: ntpd [-q] [--quiet] [-i SECONDS] [SERVER ...]");
            return 0;
        } else if (argv[i][0] == '-') {
            kx_die("invalid option");
        } else if (server_count < sizeof(servers) / sizeof(servers[0])) {
            servers[server_count++] = argv[i];
        } else {
            kx_die("too many servers");
        }
    }

    static const char *default_servers[] = {
        "time.cloudflare.com",
        "time.google.com",
        "pool.ntp.org",
    };
    if (server_count == 0) {
        memcpy(servers, default_servers, sizeof(default_servers));
        server_count = sizeof(default_servers) / sizeof(default_servers[0]);
    }

    size_t first_server = 0;
    for (;;) {
        bool synced = false;
        char error[128] = "no server responded";
        for (size_t attempt = 0; attempt < server_count; attempt++) {
            size_t index = (first_server + attempt) % server_count;
            int64_t offset_us, delay_us;
            if (sync_server(servers[index], &offset_us, &delay_us, error, sizeof(error)) == 0) {
                if (!quiet) {
                    fprintf(stderr,
                            "ntpd: synchronized with %s (offset %+lld ms, delay %lld ms)\n",
                            servers[index], (long long) (offset_us / 1000),
                            (long long) (delay_us / 1000));
                }
                first_server = (index + 1) % server_count;
                synced = true;
                break;
            }
        }

        if (once) {
            if (!synced && !quiet)
                fprintf(stderr, "ntpd: synchronization failed: %s\n", error);
            return synced ? 0 : 1;
        }
        if (!synced && !quiet)
            fprintf(stderr, "ntpd: synchronization failed: %s; retrying\n", error);
        sleep(synced ? interval : 15);
    }
}
