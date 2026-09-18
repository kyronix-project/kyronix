#pragma once
#include <stdint.h>

#define SFD_MAX_INSTANCES 8

struct signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t ssi_errno;
    int32_t ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t ssi_status;
    int32_t ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint8_t _pad[46];
};

typedef struct {
    uint64_t sigmask;
    uint8_t used;
    int fd;
    struct signalfd_siginfo pending[16];
    int pending_head;
    int pending_tail;
    int pending_count;
} signalfd_inst_t;

int sys_signalfd(int fd, const uint64_t *mask, uint32_t flags);
int64_t signalfd_read(int fd, char *buf, uint64_t len);
void signalfd_cleanup_fd(int fd);
bool signalfd_fd_is(int fd);
void signalfd_deliver(int fd, int sig);
