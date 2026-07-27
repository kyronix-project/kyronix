#pragma once

#include "fs/vfs.h"
#include <stdbool.h>
#include <stdint.h>

void procfs_init(void);

bool procfs_getdents64(vfs_node_t *dir, uint64_t *pos, void *buf, uint64_t count, int *out);
bool procfs_readlink(const char *path, char *buf, uint64_t bufsz, int *out);
bool procfs_try_pid_dir(vfs_node_t *parent, const char *name);
