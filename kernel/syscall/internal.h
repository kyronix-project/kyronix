#pragma once

#include "proc/proc.h"
#include "syscall.h"

// errno values shared across the syscall implementation modules
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define EFBIG 27
#define ENOSPC 28
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define ELOOP 40
#define EOVERFLOW 75
#define EPROTONOSUPPORT 93
#define ENOTSUP 95
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111

// current process shortcut
static inline proc_t *cur(void) { return g_current_proc; }

// shared privilege/path helpers
bool is_root(void);
bool host_priv(void);
bool path_abs(char *out, const char *in);
