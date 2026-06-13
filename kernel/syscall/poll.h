#pragma once
#include <stdint.h>

struct pollfd_s {
    int fd;
    short events;
    short revents;
};

int64_t sys_poll(struct pollfd_s* fds, uint64_t nfds, int timeout);
int64_t sys_ppoll(struct pollfd_s* fds, uint64_t nfds, void* tmo, const void* sigmask,
                  uint64_t sigsetsize);
int64_t sys_select(int nfds, void* rfds, void* wfds, void* efds, void* timeout);
int64_t sys_pselect6(int nfds, void* rfds, void* wfds, void* efds, void* timeout, void* sigmask);
