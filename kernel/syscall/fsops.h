#pragma once

#include <stdint.h>

struct utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

int64_t sys_uname(struct utsname *buf);
int64_t sys_sethostname(const char *name, uint64_t length);
uint64_t system_hostname_copy(char *out, uint64_t capacity);
int64_t sys_getcwd(char *buf, uint64_t size);
int64_t sys_chdir(const char *path);
int64_t sys_fchdir(int fd);
int64_t sys_mkdir(const char *path, uint32_t mode);
int64_t sys_rmdir(const char *path);
int64_t sys_unlink(const char *path);
int64_t sys_rename(const char *oldpath, const char *newpath);
int64_t sys_link(const char *old, const char *lnew);
int64_t sys_symlink(const char *target, const char *linkpath);
int64_t sys_truncate(const char *path, uint64_t len);
int64_t sys_ftruncate(int fd, uint64_t len);
int64_t sys_fallocate(int fd, int mode, uint64_t off, uint64_t len);
int64_t sys_statfs(const char *path, void *buf);
int64_t sys_access(const char *p, int m);
int64_t sys_umask(uint64_t mask);
