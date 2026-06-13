#pragma once
#include <stdint.h>

struct iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

struct statx_ts {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t _r;
};

struct statx {
    uint32_t stx_mask, stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink, stx_uid, stx_gid;
    uint16_t stx_mode;
    uint16_t _pad0;
    uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask;
    struct statx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint32_t stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
    uint64_t stx_mnt_id, _spare[9];
};

int64_t sys_readv(int fd, const struct iovec* iov, int n);
int64_t sys_writev(int fd, const void* iov_ptr, int n);
int64_t sys_sendfile(int outfd, int infd, uint64_t* offp, uint64_t count);
int64_t sys_preadv(int fd, const struct iovec* iov, int n, uint64_t off);
int64_t sys_pwritev(int fd, const struct iovec* iov, int n, uint64_t off);
int64_t sys_memfd_create(const char* name, uint32_t flags);
int64_t sys_copy_file_range(int infd, uint64_t* off_in, int outfd, uint64_t* off_out, uint64_t len,
                            uint32_t flags);
int64_t sys_statx(int dirfd, const char* path, int flags, uint32_t mask, struct statx* sx);
