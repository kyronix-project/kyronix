#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MOUNT_MAX 32

#define MS_RDONLY 1
#define MS_NOSUID 2
#define MS_NODEV 4
#define MS_NOEXEC 8
#define MS_REMOUNT 32

typedef struct {
    char source[64];
    char target[256];
    char fstype[16];
    uint32_t flags;
    bool used;
} mount_entry_t;

int64_t sys_mount(const char *source, const char *target, const char *fstype, uint64_t flags,
                  const void *data);
int64_t sys_umount2(const char *target, int flags);
mount_entry_t *mount_table(void);
int mount_count(void);
void mount_add(const char *source, const char *target, const char *fstype, uint32_t flags);
void mount_remove(const char *target);
mount_entry_t *mount_find(const char *target);
