#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MS_RDONLY 1
#define MS_NOSUID 2
#define MS_NODEV 4
#define MS_NOEXEC 8
#define MS_REMOUNT 32
#define MS_BIND 4096
#define MS_MOVE 8192
#define MS_REC 16384
#define MS_UNBINDABLE (1 << 17)
#define MS_PRIVATE (1 << 18)
#define MS_SLAVE (1 << 19)
#define MS_SHARED (1 << 20)

#define MNT_FORCE 1
#define MNT_DETACH 2

int64_t sys_mount(const char *source, const char *target, const char *fstype, uint64_t flags,
                  const void *data);
int64_t sys_umount2(const char *target, int flags);
int64_t sys_chroot(const char *path);
int64_t sys_pivot_root(const char *new_root, const char *put_old);
