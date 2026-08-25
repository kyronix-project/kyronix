#pragma once
#include "fs/vfs.h"

int fd_signalfd(int fd, uint64_t mask, int flags);
int64_t signalfd_read(vfs_file_t *f, char *buf, uint64_t len);
bool signalfd_pollin(vfs_file_t *f);
