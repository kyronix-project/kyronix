#pragma once
#include "fs/pipe.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern char g_cwd[512];

struct linux_stat
{
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t _pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    uint64_t st_atime_sec;
    uint64_t st_atime_nsec;
    uint64_t st_mtime_sec;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime_sec;
    uint64_t st_ctime_nsec;
    int64_t _unused[3];
};

struct linux_dirent64
{
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
} __attribute__((packed));

#define S_IFMT 0170000U
#define S_IFREG 0100000U
#define S_IFDIR 0040000U
#define S_IFLNK 0120000U
#define S_IFCHR 0020000U
#define S_IFIFO 0010000U

#define DT_UNKNOWN 0
#define DT_CHR 2
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_ACCMODE 3
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW 0400000
#define O_CLOEXEC 02000000
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH 0x1000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define VFS_TYPE_REG 1
#define VFS_TYPE_DIR 2
#define VFS_TYPE_SYM 3
#define VFS_TYPE_CHR 4

typedef struct vfs_node
{
    char name[256];
    uint8_t type;
    uint32_t mode;
    uint32_t uid, gid;
    uint32_t ino;

    uint64_t size;

    /* REG: heap buffer, capacity >= size */
    uint8_t* data;
    uint64_t capacity;

    /* DIR: linked list of children */
    struct vfs_node* children;
    struct vfs_node* next; /* next sibling */
    struct vfs_node* parent;

    /* SYM: target path */
    char* symlink;

    /* CHR: device callbacks */
    int64_t (*chr_read)(struct vfs_node*, char*, uint64_t, uint64_t);
    int64_t (*chr_write)(struct vfs_node*, const char*, uint64_t);
} vfs_node_t;

typedef struct
{
    vfs_node_t* node; /* NULL for pipe fds */
    uint64_t pos;
    int flags;
    pipe_t* pipe;
    int pipe_end; /* PIPE_END_READ or PIPE_END_WRITE */
    pipe_t* wpipe; /* non-NULL for socket fds: separate write-direction pipe */
} vfs_file_t;

#define VFS_FD_MAX 1024

void vfs_init(void);

void vfs_set_fdtable(vfs_file_t** fds);
vfs_file_t** vfs_get_fdtable(void);
void vfs_copy_fdtable(vfs_file_t** dst, vfs_file_t** src);
void vfs_free_fdtable(vfs_file_t** fds);

int fd_open(const char* path, int flags, int mode);
int fd_openat(int dirfd, const char* path, int flags, int mode);
int fd_close(int fd);
int64_t fd_read(int fd, void* buf, uint64_t len);
int64_t fd_write(int fd, const void* buf, uint64_t len);
int64_t fd_lseek(int fd, int64_t off, int whence);
int fd_stat(const char* path, struct linux_stat* st);
int fd_lstat(const char* path, struct linux_stat* st);
int fd_fstatat(int dirfd, const char* path, struct linux_stat* st, int flags);
int fd_fstat(int fd, struct linux_stat* st);
int fd_getdents64(int fd, void* buf, uint64_t count);
int fd_readlink(const char* path, char* buf, uint64_t bufsz);
int fd_ioctl(int fd, uint64_t req, uint64_t arg);
int fd_fcntl(int fd, int cmd, uint64_t arg);
int fd_dup(int oldfd);
int fd_dup2(int oldfd, int newfd);
bool fd_valid(int fd);
vfs_node_t* fd_get_node(int fd);
vfs_file_t* fd_get_file(int fd);
int64_t     fd_pread(int fd, void* buf, uint64_t len, uint64_t off);
int64_t     fd_pwrite(int fd, const void* buf, uint64_t len, uint64_t off);
bool fd_pollin(int fd);
bool fd_pollout(int fd);
int fd_pipe(int pipefd[2]);
int fd_socketpair(int sv[2]);

vfs_node_t* vfs_lookup(const char* path);
vfs_node_t* vfs_lookup_nofollow(const char* path);
vfs_node_t* vfs_mkdir_p(const char* path, uint32_t mode);
vfs_node_t* vfs_create_file(const char* path, uint32_t mode, const void* data, uint64_t size);
vfs_node_t* vfs_create_symlink(const char* path, const char* target);
vfs_node_t* vfs_create_chr(const char* path, int64_t (*rfn)(vfs_node_t*, char*, uint64_t, uint64_t),
                           int64_t (*wfn)(vfs_node_t*, const char*, uint64_t));

int vfs_mkdir(const char* path, uint32_t mode);
int vfs_unlink(const char* path);
int vfs_rmdir(const char* path);
int vfs_rename(const char* oldpath, const char* newpath);
int vfs_link(const char* oldpath, const char* newpath);
int vfs_chmod(const char* path, uint32_t mode);
int vfs_fchmod(int fd, uint32_t mode);
int vfs_chown(const char* path, uint32_t uid, uint32_t gid);
int vfs_lchown(const char* path, uint32_t uid, uint32_t gid);
int vfs_fchown(int fd, uint32_t uid, uint32_t gid);
int vfs_mknod(const char* path, uint32_t mode, uint64_t dev);
char* vfs_node_abspath(vfs_node_t* n, char* buf, size_t sz);
int   at_resolve(int dirfd, const char* path, char* out, size_t sz);
int   fd_dup3(int oldfd, int newfd, int flags);
