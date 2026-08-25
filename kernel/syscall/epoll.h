#pragma once
#include <stdbool.h>
#include <stdint.h>

struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

int64_t sys_epoll_create1(int flags);
int64_t sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev);
int64_t sys_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

bool epoll_fd_is_handle(int fd);
bool epoll_fd_pollin(int fd);
