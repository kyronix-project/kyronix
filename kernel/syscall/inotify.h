#pragma once
#include <stdbool.h>
#include <stdint.h>

#define INOTIFY_MAX_WATCHES 128
#define INOTIFY_MAX_INSTANCES 16
#define INOTIFY_BUFSZ 4096

#define IN_ACCESS        0x00000001
#define IN_MODIFY        0x00000002
#define IN_ATTRIB        0x00000004
#define IN_CLOSE_WRITE   0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN          0x00000020
#define IN_MOVED_FROM    0x00000040
#define IN_MOVED_TO      0x00000080
#define IN_CREATE        0x00000100
#define IN_DELETE        0x00000200
#define IN_DELETE_SELF   0x00000400
#define IN_MOVE_SELF     0x00000800
#define IN_UNMOUNT       0x00002000
#define IN_Q_OVERFLOW    0x00004000
#define IN_IGNORED       0x00008000
#define IN_ONLYDIR       0x01000000
#define IN_DONT_FOLLOW   0x02000000
#define IN_EXCL_UNLINK   0x04000000
#define IN_MASK_ADD      0x20000000
#define IN_ISDIR         0x40000000
#define IN_ONESHOT       0x80000000

struct inotify_event {
    int32_t wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
    char name[];
};

typedef struct inotify_watch {
    int32_t wd;
    uint32_t mask;
    char path[512];
    uint8_t used;
} inotify_watch_t;

typedef struct inotify_instance {
    inotify_watch_t watches[INOTIFY_MAX_WATCHES];
    uint8_t ring[INOTIFY_BUFSZ];
    uint32_t head;
    uint32_t tail;
    uint32_t overflow;
    uint8_t used;
    int flags;
} inotify_instance_t;

int sys_inotify_init(void);
int sys_inotify_init1(int flags);
int sys_inotify_add_watch(int fd, const char *pathname, uint32_t mask);
int sys_inotify_rm_watch(int fd, int wd);
int64_t inotify_fd_read(int fd, char *buf, uint64_t len);
void inotify_cleanup_fd(int fd);
void inotify_emit(int fd, int32_t wd, uint32_t mask, uint32_t cookie, const char *name);
int inotify_fd_open(int fd, int flags);
void inotify_fd_close(int fd);
bool inotify_fd_is(int fd);
