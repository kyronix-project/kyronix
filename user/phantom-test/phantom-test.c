#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SYS_PHANTOM_MODE 507
#define SYS_PHANTOM_READ 508
#define PHANTOM_AUDIT 1
#define PHANTOM_EVENT_RING 64
#define PHANTOM_DETAIL_MAX 48

typedef struct {
    uint64_t sequence, tick;
    uint32_t pid, jail_id, kind, flags;
    uint64_t address, instruction;
    char detail[PHANTOM_DETAIL_MAX];
} phantom_event_t;

int main(void) {
    printf("Phantom telemetry self-test\n");
    long mode = syscall(SYS_PHANTOM_MODE, PHANTOM_AUDIT);
    if (mode < 0) {
        fprintf(stderr, "[FAIL] enable audit: %s\n", strerror(errno));
        return 1;
    }
    printf("[ OK ] audit mode enabled -> %ld\n", mode);

    int fd = open("/phantom-test/no-such-file", O_RDONLY);
    if (fd >= 0) close(fd);
    printf("[ OK ] controlled VFS miss\n");
    (void) access("/phantom-test/no-such-file", F_OK);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        (void) connect(fd, (struct sockaddr *) &addr, sizeof(addr));
        close(fd);
        printf("[ OK ] controlled TCP connect attempted\n");
    } else {
        printf("[INFO] socket unavailable: %s\n", strerror(errno));
    }

    phantom_event_t events[PHANTOM_EVENT_RING];
    long n = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (n < 0) {
        fprintf(stderr, "[FAIL] read event ring: %s\n", strerror(errno));
        return 1;
    }
    if (n == 0) {
        fprintf(stderr, "[FAIL] event ring is empty\n");
        return 1;
    }
    for (long i = 0; i < n; i++)
        printf("event #%llu kind=%u pid=%u flags=0x%x detail=%s\n",
               (unsigned long long) events[i].sequence, events[i].kind,
               events[i].pid, events[i].flags, events[i].detail);
    printf("Phantom self-test: PASS (%ld events)\n", n);
    return 0;
}
