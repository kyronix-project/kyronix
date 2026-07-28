#pragma once

#include "fs/vfs.h"
#include <stdint.h>

int64_t sys_init_module(const void *image, uint64_t length, const char *params);
int64_t sys_finit_module(int fd, const char *params, uint32_t flags);
int64_t sys_delete_module(const char *name, uint32_t flags);

int module_load_path(const char *path);
int64_t module_proc_read(vfs_node_t *node, char *buf, uint64_t len, uint64_t off);
