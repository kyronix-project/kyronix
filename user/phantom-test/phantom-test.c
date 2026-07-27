#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define SYS_JAIL_CREATE 500
#define SYS_JAIL_REMOVE 504
#define SYS_PHANTOM_MODE 507
#define SYS_PHANTOM_READ 508
#define SYS_PHANTOM_CLONE 509
#define SYS_PHANTOM_CONTROL 510
#define PHANTOM_AUDIT 1
#define PHANTOM_QUARANTINE 3
#define PHANTOM_EVENT_FAULT 1
#define PHANTOM_EVENT_NETWORK 3
#define PHANTOM_EVENT_CLONE 5
#define PHANTOM_EVENT_QUARANTINE 6
#define PHANTOM_CLONEF_COMMITTED 0x04
#define PHANTOM_CLONEF_FAILED 0x08
#define PHANTOM_CLONEF_FAULT 0x10
#define PHANTOM_CLONEF_WORKER 0x20
#define PHANTOM_QUARANTINEF_HOLD 0x01
#define PHANTOM_QUARANTINEF_RESUME 0x02
#define PHANTOM_QUARANTINEF_TERMINATE 0x04
#define PHANTOM_QUARANTINEF_READY 0x08
#define PHANTOM_CTL_STATUS 0
#define PHANTOM_CTL_RESUME 1
#define PHANTOM_CTL_TERMINATE 2
#define PHANTOM_CTL_LAST_SANDBOX 3
#define PHANTOM_EVENT_RING 64
#define PHANTOM_DETAIL_MAX 48

typedef struct {
    char root[256];
    char name[32];
    uint32_t flags;
    uint32_t max_procs;
    uint32_t attach;
} kjail_conf_t;

typedef struct {
    uint64_t sequence, tick;
    uint32_t pid, jail_id, kind, flags;
    uint64_t address, instruction;
    char detail[PHANTOM_DETAIL_MAX];
} phantom_event_t;

static void run_quarantine_offender(void) {
    for (int i = 0; i < 10; i++)
        (void) access("/phantom-quarantine/no-such-file", F_OK);

    long branch = syscall(39); /* deferred clone + quarantine safe point */
    if (branch == 0) {
        int fake = open("/etc/hostname", O_RDONLY);
        if (fake < 0) _exit(31);
        close(fake);
        _exit(0);
    }
    if (branch < 0) _exit(32);

    long sandbox_pid =
        syscall(SYS_PHANTOM_CONTROL, PHANTOM_CTL_LAST_SANDBOX, (pid_t) branch);
    if (sandbox_pid <= 0) _exit(33);
    int status = 0;
    if (waitpid((pid_t) sandbox_pid, &status, 0) != (pid_t) sandbox_pid ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        _exit(34);
    _exit(0);
}

static void run_fault_offender(void) {
    volatile uint8_t *page =
        mmap(NULL, 4096, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) _exit(41);
    page[0] = 1;
    if (mprotect((void *) page, 4096, PROT_READ) < 0) _exit(42);

    /*
     * The source timeline stops on this write. Its fault clone gets a private
     * writable copy and resumes at this exact instruction.
     */
    page[0] = 2;
    if (page[0] != 2) _exit(43);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) _exit(44);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(443);
    inet_pton(AF_INET, "203.0.113.9", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) _exit(45);
    close(fd);
    _exit(0);
}

static void run_execute_fault_offender(void) {
    uint8_t *page =
        mmap(NULL, 4096, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) _exit(51);
    page[0] = 0xc3; /* ret */
    if (mprotect(page, 4096, PROT_READ) < 0) _exit(52);

    void (*entry)(void) = (void (*)(void)) (uintptr_t) page;
    entry();

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) _exit(53);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8443);
    inet_pton(AF_INET, "198.51.100.19", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) _exit(54);
    close(fd);
    _exit(0);
}

static long wait_for_quarantine(pid_t source) {
    for (int i = 0; i < 5000; i++) {
        long sandbox =
            syscall(SYS_PHANTOM_CONTROL, PHANTOM_CTL_STATUS, source);
        if (sandbox > 0) return sandbox;
        usleep(1000);
    }
    return -1;
}

static int wait_for_fault_telemetry(uint64_t cursor, pid_t source,
                                    pid_t sandbox) {
    phantom_event_t events[PHANTOM_EVENT_RING];
    for (int attempt = 0; attempt < 5000; attempt++) {
        long n = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
        if (n < 0) return -1;
        int fault = 0, clone = 0, ready = 0, network = 0;
        for (long i = 0; i < n; i++) {
            if (events[i].sequence <= cursor) continue;
            if (events[i].kind == PHANTOM_EVENT_FAULT &&
                events[i].pid == (uint32_t) source)
                fault = 1;
            if (events[i].kind == PHANTOM_EVENT_CLONE &&
                events[i].pid == (uint32_t) source &&
                events[i].address == (uint64_t) sandbox &&
                (events[i].flags &
                 (PHANTOM_CLONEF_COMMITTED | PHANTOM_CLONEF_FAULT |
                  PHANTOM_CLONEF_WORKER)) ==
                    (PHANTOM_CLONEF_COMMITTED | PHANTOM_CLONEF_FAULT |
                     PHANTOM_CLONEF_WORKER))
                clone = 1;
            if (events[i].kind == PHANTOM_EVENT_QUARANTINE &&
                events[i].pid == (uint32_t) source &&
                events[i].address == (uint64_t) sandbox &&
                (events[i].flags & PHANTOM_QUARANTINEF_READY))
                ready = 1;
            if (events[i].kind == PHANTOM_EVENT_NETWORK &&
                events[i].pid == (uint32_t) sandbox)
                network = 1;
        }
        if (fault && clone && ready && network) return 0;
        usleep(1000);
    }
    return 1;
}

int main(void) {
    printf("Phantom telemetry self-test\n");
    long mode = syscall(SYS_PHANTOM_MODE, PHANTOM_AUDIT);
    if (mode < 0) {
        fprintf(stderr, "[FAIL] enable audit: %s\n", strerror(errno));
        return 1;
    }
    printf("[ OK ] audit mode enabled -> %ld\n", mode);

    phantom_event_t events[PHANTOM_EVENT_RING];
    long existing = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (existing < 0) {
        fprintf(stderr, "[FAIL] establish event cursor: %s\n", strerror(errno));
        return 1;
    }
    uint64_t run_cursor = existing ? events[existing - 1].sequence : 0;

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

    long n = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (n < 0) {
        fprintf(stderr, "[FAIL] read event ring: %s\n", strerror(errno));
        return 1;
    }
    long run_events = 0;
    for (long i = 0; i < n; i++) {
        if (events[i].sequence <= run_cursor) continue;
        run_events++;
        printf("event #%llu kind=%u pid=%u flags=0x%x detail=%s\n",
               (unsigned long long) events[i].sequence, events[i].kind,
               events[i].pid, events[i].flags, events[i].detail);
    }
    if (run_events == 0) {
        fprintf(stderr, "[FAIL] no events captured for this run\n");
        return 1;
    }

    int inherited_file = open("/etc/os-release", O_RDONLY);
    int inherited_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (inherited_file < 0 || inherited_socket < 0) {
        fprintf(stderr, "[FAIL] prepare inherited descriptors: %s\n", strerror(errno));
        return 1;
    }
    long child = syscall(SYS_PHANTOM_CLONE);
    if (child < 0) {
        fprintf(stderr, "[FAIL] COW phantom clone: %s\n", strerror(errno));
        return 1;
    }
    printf("[ OK ] COW phantom clone -> %ld\n", child);
    if (child == 0) {
        char probe;
        errno = 0;
        if (read(inherited_file, &probe, 1) >= 0 || errno != EBADF) {
            fprintf(stderr, "[FAIL] inherited host file was not closed\n");
            return 1;
        }
        if (write(inherited_socket, "x", 1) != 1) {
            fprintf(stderr, "[FAIL] inherited socket was not phantomized\n");
            return 1;
        }
        printf("[ OK ] inherited descriptors isolated\n");

        int fake = open("/etc/hostname", O_RDONLY);
        if (fake < 0) {
            fprintf(stderr, "[FAIL] sandbox VFS overlay: %s\n", strerror(errno));
            return 1;
        }
        close(fake);
        printf("[ OK ] sandbox VFS overlay visible\n");

        fake = open("/etc/ssl/phantom.key", O_RDONLY);
        if (fake < 0) {
            fprintf(stderr, "[FAIL] dummy crypto material: %s\n", strerror(errno));
            return 1;
        }
        char key[32] = {0};
        ssize_t key_len = read(fake, key, sizeof(key) - 1);
        close(fake);
        if (key_len < 17 || memcmp(key, "PHANTOM-DUMMY-KEY", 17) != 0) {
            fprintf(stderr, "[FAIL] dummy crypto material mismatch\n");
            return 1;
        }
        printf("[ OK ] dummy crypto material visible\n");

        fake = socket(AF_INET, SOCK_STREAM, 0);
        if (fake < 0) {
            fprintf(stderr, "[FAIL] sandbox socket: %s\n", strerror(errno));
            return 1;
        }
        struct sockaddr_in sandbox_addr;
        memset(&sandbox_addr, 0, sizeof(sandbox_addr));
        sandbox_addr.sin_family = AF_INET;
        sandbox_addr.sin_port = htons(443);
        inet_pton(AF_INET, "198.51.100.7", &sandbox_addr.sin_addr);
        if (connect(fake, (struct sockaddr *) &sandbox_addr, sizeof(sandbox_addr)) < 0) {
            fprintf(stderr, "[FAIL] spoofed network ACK: %s\n", strerror(errno));
            close(fake);
            return 1;
        }
        close(fake);
        printf("[ OK ] spoofed network ACK\n");

        printf("Phantom sandbox-child test: PASS\n");
        return 0;
    }

    char parent_probe;
    if (read(inherited_file, &parent_probe, 1) != 1) {
        fprintf(stderr, "[FAIL] parent descriptor damaged by clone\n");
        return 1;
    }
    close(inherited_file);
    close(inherited_socket);
    printf("[ OK ] parent descriptors preserved\n");

    int child_status = 0;
    if (waitpid((pid_t) child, &child_status, 0) != (pid_t) child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        fprintf(stderr, "[FAIL] sandbox child did not exit cleanly\n");
        return 1;
    }
    printf("[ OK ] sandbox child reaped\n");

    long committed_n = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (committed_n < 0) {
        fprintf(stderr, "[FAIL] read clone commit telemetry: %s\n", strerror(errno));
        return 1;
    }
    int committed = 0;
    for (long i = 0; i < committed_n; i++) {
        if (events[i].sequence > run_cursor &&
            events[i].kind == PHANTOM_EVENT_CLONE &&
            events[i].address == (uint64_t) child &&
            (events[i].flags & PHANTOM_CLONEF_COMMITTED))
            committed = 1;
    }
    if (!committed) {
        fprintf(stderr, "[FAIL] sandbox was not atomically committed\n");
        return 1;
    }
    printf("[ OK ] sandbox atomically published\n");

    long before_auto = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (before_auto < 0) {
        fprintf(stderr, "[FAIL] establish auto-clone cursor: %s\n", strerror(errno));
        return 1;
    }
    uint64_t auto_cursor = before_auto ? events[before_auto - 1].sequence : run_cursor;

    long trap = syscall(SYS_PHANTOM_MODE, 2);
    if (trap < 0) {
        fprintf(stderr, "[FAIL] enable trap mode: %s\n", strerror(errno));
        return 1;
    }
    for (int i = 0; i < 10; i++)
        (void) access("/phantom-auto/no-such-file", F_OK);
    long safe_pid = syscall(39); /* next syscall is the deferred clone safe point */
    if (safe_pid == 0) {
        int fake = open("/etc/hostname", O_RDONLY);
        if (fake < 0) {
            fprintf(stderr, "[FAIL] deferred sandbox VFS: %s\n", strerror(errno));
            return 1;
        }
        close(fake);
        printf("[ OK ] deferred auto-clone sandbox active\n");
        printf("Phantom deferred-child test: PASS\n");
        return 0;
    } else if (safe_pid > 0) {
        printf("[ OK ] deferred auto-clone parent continued\n");
    } else {
        fprintf(stderr, "[FAIL] deferred safe point: %s\n", strerror(errno));
        return 1;
    }

    long after_auto = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (after_auto < 0) {
        fprintf(stderr, "[FAIL] read auto-clone telemetry: %s\n", strerror(errno));
        return 1;
    }
    pid_t auto_child = 0;
    for (long i = 0; i < after_auto; i++) {
        if (events[i].sequence > auto_cursor &&
            events[i].kind == PHANTOM_EVENT_CLONE &&
            (events[i].flags & PHANTOM_CLONEF_COMMITTED) &&
            events[i].address != 0)
            auto_child = (pid_t) events[i].address;
    }
    if (!auto_child) {
        fprintf(stderr, "[FAIL] deferred clone PID missing from telemetry\n");
        return 1;
    }
    printf("[ OK ] deferred sandbox atomically published\n");
    child_status = 0;
    if (waitpid(auto_child, &child_status, 0) != auto_child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        fprintf(stderr, "[FAIL] deferred child did not exit cleanly\n");
        return 1;
    }
    printf("[ OK ] deferred child reaped\n");

    uint32_t fill_jails[32];
    int fill_count = 0;
    kjail_conf_t fill_conf;
    memset(&fill_conf, 0, sizeof(fill_conf));
    strcpy(fill_conf.root, "/");
    strcpy(fill_conf.name, "phantom-fill");
    int fill_errno = ENOSPC;
    while (fill_count < (int) (sizeof(fill_jails) / sizeof(fill_jails[0]))) {
        long jid = syscall(SYS_JAIL_CREATE, &fill_conf);
        if (jid < 0) {
            fill_errno = errno;
            break;
        }
        fill_jails[fill_count++] = (uint32_t) jid;
    }
    if (fill_count < (int) (sizeof(fill_jails) / sizeof(fill_jails[0])) &&
        fill_errno != ENOSPC) {
        for (int i = 0; i < fill_count; i++)
            (void) syscall(SYS_JAIL_REMOVE, fill_jails[i]);
        fprintf(stderr, "[FAIL] could not saturate jail table: %s\n", strerror(fill_errno));
        return 1;
    }

    long fail_before = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (fail_before < 0) {
        for (int i = 0; i < fill_count; i++)
            (void) syscall(SYS_JAIL_REMOVE, fill_jails[i]);
        fprintf(stderr, "[FAIL] establish fail-closed cursor: %s\n", strerror(errno));
        return 1;
    }
    uint64_t fail_cursor =
        fail_before > 0 ? events[fail_before - 1].sequence : run_cursor;
    errno = 0;
    long rejected = syscall(SYS_PHANTOM_CLONE);
    int reject_errno = errno;

    for (int i = 0; i < fill_count; i++)
        (void) syscall(SYS_JAIL_REMOVE, fill_jails[i]);

    if (rejected >= 0 || reject_errno != ENOMEM) {
        fprintf(stderr, "[FAIL] saturated sandbox setup did not fail closed\n");
        return 1;
    }

    long failed_n = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (failed_n < 0) {
        fprintf(stderr, "[FAIL] read fail-closed telemetry: %s\n", strerror(errno));
        return 1;
    }
    int failed_closed = 0;
    for (long i = 0; i < failed_n; i++) {
        if (events[i].sequence > fail_cursor &&
            events[i].kind == PHANTOM_EVENT_CLONE &&
            (events[i].flags & PHANTOM_CLONEF_FAILED))
            failed_closed = 1;
    }
    if (!failed_closed) {
        fprintf(stderr, "[FAIL] fail-closed event missing\n");
        return 1;
    }

    child_status = 0;
    errno = 0;
    if (waitpid(-1, &child_status, WNOHANG) != -1 || errno != ECHILD) {
        fprintf(stderr, "[FAIL] unpublished embryo escaped rollback\n");
        return 1;
    }
    printf("[ OK ] sandbox setup failure rolled back safely\n");

    long quarantine_before = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (quarantine_before < 0) {
        fprintf(stderr, "[FAIL] establish quarantine cursor: %s\n", strerror(errno));
        return 1;
    }
    uint64_t quarantine_cursor =
        quarantine_before ? events[quarantine_before - 1].sequence : run_cursor;

    if (syscall(SYS_PHANTOM_MODE, PHANTOM_QUARANTINE) != PHANTOM_QUARANTINE) {
        fprintf(stderr, "[FAIL] enable quarantine mode: %s\n", strerror(errno));
        return 1;
    }

    pid_t offender = fork();
    if (offender < 0) {
        fprintf(stderr, "[FAIL] create quarantine offender: %s\n", strerror(errno));
        return 1;
    }
    if (offender == 0) run_quarantine_offender();

    long quarantine_sandbox = wait_for_quarantine(offender);
    if (quarantine_sandbox <= 0) {
        fprintf(stderr, "[FAIL] source timeline was not quarantined\n");
        return 1;
    }
    child_status = 0;
    if (waitpid(offender, &child_status, WNOHANG) != 0) {
        fprintf(stderr, "[FAIL] quarantined source remained schedulable\n");
        return 1;
    }
    printf("[ OK ] source timeline quarantined (sandbox=%ld)\n",
           quarantine_sandbox);

    if (syscall(SYS_PHANTOM_CONTROL, PHANTOM_CTL_RESUME, offender) != 0) {
        fprintf(stderr, "[FAIL] resume quarantined source: %s\n", strerror(errno));
        return 1;
    }
    child_status = 0;
    if (waitpid(offender, &child_status, 0) != offender ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        fprintf(stderr, "[FAIL] resumed source or sandbox branch failed\n");
        return 1;
    }
    printf("[ OK ] sandbox-only branch completed before source resume\n");

    if (syscall(SYS_PHANTOM_MODE, PHANTOM_QUARANTINE) != PHANTOM_QUARANTINE) {
        fprintf(stderr, "[FAIL] reset quarantine mode: %s\n", strerror(errno));
        return 1;
    }
    offender = fork();
    if (offender < 0) {
        fprintf(stderr, "[FAIL] create terminate offender: %s\n", strerror(errno));
        return 1;
    }
    if (offender == 0) run_quarantine_offender();

    quarantine_sandbox = wait_for_quarantine(offender);
    if (quarantine_sandbox <= 0) {
        fprintf(stderr, "[FAIL] terminate source was not quarantined\n");
        return 1;
    }
    if (syscall(SYS_PHANTOM_CONTROL, PHANTOM_CTL_TERMINATE, offender) != 0) {
        fprintf(stderr, "[FAIL] terminate quarantined source: %s\n", strerror(errno));
        return 1;
    }
    child_status = 0;
    if (waitpid(offender, &child_status, 0) != offender ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) == 0) {
        fprintf(stderr, "[FAIL] quarantined source was not terminated\n");
        return 1;
    }
    printf("[ OK ] quarantined source terminated by controller\n");

    long fault_before = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (fault_before < 0) {
        fprintf(stderr, "[FAIL] establish fault cursor: %s\n", strerror(errno));
        return 1;
    }
    uint64_t fault_cursor =
        fault_before ? events[fault_before - 1].sequence : run_cursor;
    if (syscall(SYS_PHANTOM_MODE, PHANTOM_QUARANTINE) != PHANTOM_QUARANTINE) {
        fprintf(stderr, "[FAIL] reset fault quarantine mode: %s\n", strerror(errno));
        return 1;
    }

    offender = fork();
    if (offender < 0) {
        fprintf(stderr, "[FAIL] create fault offender: %s\n", strerror(errno));
        return 1;
    }
    if (offender == 0) run_fault_offender();

    quarantine_sandbox = wait_for_quarantine(offender);
    if (quarantine_sandbox <= 0) {
        fprintf(stderr, "[FAIL] faulting source was not quarantined\n");
        return 1;
    }
    int fault_telemetry =
        wait_for_fault_telemetry(fault_cursor, offender,
                                 (pid_t) quarantine_sandbox);
    if (fault_telemetry != 0) {
        fprintf(stderr, "[FAIL] fault sandbox did not resume through spoofed network\n");
        return 1;
    }
    printf("[ OK ] fault worker resumed exact instruction (sandbox=%ld)\n",
           quarantine_sandbox);

    errno = 0;
    if (syscall(SYS_PHANTOM_CONTROL, PHANTOM_CTL_RESUME, offender) != -1 ||
        errno != EPERM) {
        fprintf(stderr, "[FAIL] unsafe fault-source resume was not rejected\n");
        return 1;
    }
    printf("[ OK ] unsafe fault-source resume rejected\n");

    if (syscall(SYS_PHANTOM_CONTROL, PHANTOM_CTL_TERMINATE, offender) != 0) {
        fprintf(stderr, "[FAIL] terminate faulting source: %s\n", strerror(errno));
        return 1;
    }
    child_status = 0;
    if (waitpid(offender, &child_status, 0) != offender ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) == 0) {
        fprintf(stderr, "[FAIL] faulting source was not terminated\n");
        return 1;
    }
    printf("[ OK ] faulting source terminated after sandbox capture\n");

    fault_before = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (fault_before < 0) {
        fprintf(stderr, "[FAIL] establish execute-fault cursor: %s\n",
                strerror(errno));
        return 1;
    }
    fault_cursor = fault_before ? events[fault_before - 1].sequence : run_cursor;
    if (syscall(SYS_PHANTOM_MODE, PHANTOM_QUARANTINE) != PHANTOM_QUARANTINE) {
        fprintf(stderr, "[FAIL] reset execute-fault mode: %s\n", strerror(errno));
        return 1;
    }

    offender = fork();
    if (offender < 0) {
        fprintf(stderr, "[FAIL] create execute-fault offender: %s\n",
                strerror(errno));
        return 1;
    }
    if (offender == 0) run_execute_fault_offender();

    quarantine_sandbox = wait_for_quarantine(offender);
    if (quarantine_sandbox <= 0) {
        fprintf(stderr, "[FAIL] execute-fault source was not quarantined\n");
        return 1;
    }
    fault_telemetry =
        wait_for_fault_telemetry(fault_cursor, offender,
                                 (pid_t) quarantine_sandbox);
    if (fault_telemetry != 0) {
        fprintf(stderr, "[FAIL] execute-fault sandbox did not resume\n");
        return 1;
    }
    printf("[ OK ] NX worker resumed exact instruction (sandbox=%ld)\n",
           quarantine_sandbox);

    errno = 0;
    if (syscall(SYS_PHANTOM_CONTROL, PHANTOM_CTL_RESUME, offender) != -1 ||
        errno != EPERM) {
        fprintf(stderr, "[FAIL] unsafe execute-fault resume was not rejected\n");
        return 1;
    }
    if (syscall(SYS_PHANTOM_CONTROL, PHANTOM_CTL_TERMINATE, offender) != 0) {
        fprintf(stderr, "[FAIL] terminate execute-fault source: %s\n",
                strerror(errno));
        return 1;
    }
    child_status = 0;
    if (waitpid(offender, &child_status, 0) != offender ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) == 0) {
        fprintf(stderr, "[FAIL] execute-fault source was not terminated\n");
        return 1;
    }
    printf("[ OK ] execute-fault source terminated after sandbox capture\n");

    fill_count = 0;
    fill_errno = ENOSPC;
    while (fill_count < (int) (sizeof(fill_jails) / sizeof(fill_jails[0]))) {
        long jid = syscall(SYS_JAIL_CREATE, &fill_conf);
        if (jid < 0) {
            fill_errno = errno;
            break;
        }
        fill_jails[fill_count++] = (uint32_t) jid;
    }
    if (fill_count < (int) (sizeof(fill_jails) / sizeof(fill_jails[0])) &&
        fill_errno != ENOSPC) {
        for (int i = 0; i < fill_count; i++)
            (void) syscall(SYS_JAIL_REMOVE, fill_jails[i]);
        fprintf(stderr, "[FAIL] could not saturate worker jail table: %s\n",
                strerror(fill_errno));
        return 1;
    }

    fault_before = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (fault_before < 0) {
        for (int i = 0; i < fill_count; i++)
            (void) syscall(SYS_JAIL_REMOVE, fill_jails[i]);
        fprintf(stderr, "[FAIL] establish worker-failure cursor: %s\n",
                strerror(errno));
        return 1;
    }
    fault_cursor = fault_before ? events[fault_before - 1].sequence : run_cursor;
    if (syscall(SYS_PHANTOM_MODE, PHANTOM_QUARANTINE) != PHANTOM_QUARANTINE) {
        for (int i = 0; i < fill_count; i++)
            (void) syscall(SYS_JAIL_REMOVE, fill_jails[i]);
        fprintf(stderr, "[FAIL] reset worker-failure mode: %s\n",
                strerror(errno));
        return 1;
    }

    offender = fork();
    if (offender < 0) {
        for (int i = 0; i < fill_count; i++)
            (void) syscall(SYS_JAIL_REMOVE, fill_jails[i]);
        fprintf(stderr, "[FAIL] create worker-failure offender: %s\n",
                strerror(errno));
        return 1;
    }
    if (offender == 0) run_fault_offender();

    child_status = 0;
    if (waitpid(offender, &child_status, 0) != offender ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) == 0) {
        for (int i = 0; i < fill_count; i++)
            (void) syscall(SYS_JAIL_REMOVE, fill_jails[i]);
        fprintf(stderr, "[FAIL] failed worker sandbox did not terminate source\n");
        return 1;
    }
    for (int i = 0; i < fill_count; i++)
        (void) syscall(SYS_JAIL_REMOVE, fill_jails[i]);

    long worker_failed_n =
        syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (worker_failed_n < 0) {
        fprintf(stderr, "[FAIL] read worker-failure telemetry: %s\n",
                strerror(errno));
        return 1;
    }
    int worker_failed_closed = 0;
    for (long i = 0; i < worker_failed_n; i++) {
        if (events[i].sequence > fault_cursor &&
            events[i].pid == (uint32_t) offender &&
            events[i].kind == PHANTOM_EVENT_CLONE &&
            (events[i].flags &
             (PHANTOM_CLONEF_FAILED | PHANTOM_CLONEF_WORKER)) ==
                (PHANTOM_CLONEF_FAILED | PHANTOM_CLONEF_WORKER))
            worker_failed_closed = 1;
    }
    if (!worker_failed_closed) {
        fprintf(stderr, "[FAIL] worker fail-closed event missing\n");
        return 1;
    }
    printf("[ OK ] worker sandbox failure rolled back and terminated source\n");

    if (syscall(SYS_PHANTOM_MODE, PHANTOM_AUDIT) != PHANTOM_AUDIT) {
        fprintf(stderr, "[FAIL] restore audit mode: %s\n", strerror(errno));
        return 1;
    }
    long quarantine_n = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (quarantine_n < 0) {
        fprintf(stderr, "[FAIL] read quarantine telemetry: %s\n", strerror(errno));
        return 1;
    }
    int saw_hold = 0, saw_resume = 0, saw_terminate = 0;
    for (long i = 0; i < quarantine_n; i++) {
        if (events[i].sequence <= quarantine_cursor ||
            events[i].kind != PHANTOM_EVENT_QUARANTINE)
            continue;
        if (events[i].flags & PHANTOM_QUARANTINEF_HOLD) saw_hold = 1;
        if (events[i].flags & PHANTOM_QUARANTINEF_RESUME) saw_resume = 1;
        if (events[i].flags & PHANTOM_QUARANTINEF_TERMINATE) saw_terminate = 1;
    }
    if (!saw_hold || !saw_resume || !saw_terminate) {
        fprintf(stderr, "[FAIL] incomplete quarantine telemetry\n");
        return 1;
    }
    printf("[ OK ] quarantine hold/resume/terminate telemetry\n");

    long final_n = syscall(SYS_PHANTOM_READ, events, PHANTOM_EVENT_RING);
    if (final_n < 0) {
        fprintf(stderr, "[FAIL] final event read: %s\n", strerror(errno));
        return 1;
    }
    run_events = 0;
    for (long i = 0; i < final_n; i++)
        if (events[i].sequence > run_cursor) run_events++;
    printf("Phantom self-test: PASS (%ld current-run events)\n", run_events);
    return 0;
}
