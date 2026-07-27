#include "syscall.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/pit.h"
#include "arch/x86_64/syscall_setup.h"
#include "cred.h"
#include "crypto/chacha20.h"
#include "drivers/acpi.h"
#include "epoll.h"
#include "exec/process.h"
#include "file.h"
#include "fs/inet_socket.h"
#include "fs/vfs.h"
#include "fs/vfs_internal.h"
#include "fsops.h"
#include "futex.h"
#include "internal.h"
#include "jailsys.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mem.h"
#include "phantom.h"
#include "mm/pmm.h"
#include "mm/shm.h"
#include "mm/vmm.h"
#include "mount.h"
#include "poll.h"
#include "proc/jail.h"
#include "proc/proc.h"
#include "security/phantom.h"
#include "proc/signal.h"
#include "proc/smp.h"
#include "procctl.h"
#include "ptrace.h"
#include "sig.h"
#include "socket.h"
#include "time.h"
#include "version.h"

static bool copy_user_path(char *out, const char *in) {
    if (!in) return false;
    for (size_t i = 0; i < 511; i++) {
        const char *a = in + i;
        if (i == 0 || ((uint64_t) (uintptr_t) a & 0xFFF) == 0) {
            if (!uptr_ok(a, 1)) return false;
        }
        char c = *a;
        out[i] = c;
        if (!c) return true;
    }
    out[511] = '\0';
    return true;
}

/* resolve a user path to an absolute one in out[512]. returns false
                if the user pointer is invalid; on success out is always non-empty */
bool path_abs(char *out, const char *in) {
    char tmp[512];
    if (!copy_user_path(tmp, in)) return false;
    const char *root = jail_root_current();
    if (tmp[0] == '/') {
        if (root[0])
            snprintf(out, 512, "%s%s", root, tmp);
        else
            memcpy(out, tmp, sizeof(tmp));
    } else {
        proc_t *p = cur();
        const char *cwd = (p && p->cwd[0]) ? p->cwd : "/";
        size_t cl = strlen(cwd);
        if (cl >= 511) {
            out[0] = '/';
            out[1] = '\0';
        } else {
            memcpy(out, cwd, cl);
            if (out[cl - 1] != '/') out[cl++] = '/';
            strncpy(out + cl, tmp, 511 - cl);
            out[511] = '\0';
        }
    }
    if (root[0]) jail_canon_clamp(out, 512, root);
    return true;
}

static int64_t sys_set_tid_address(void *p) {
    if (p && !uptr_ok_w(p, sizeof(uint32_t))) return -(int64_t) EFAULT;
    proc_t *proc = cur();
    if (proc) proc->cleartid_addr = (uint32_t *) p;
    return (int64_t) sys_getpid();
}
static int64_t sys_set_robust_list(void *h, uint64_t l) {
    (void) h;
    (void) l;
    return 0;
}

static int64_t sys_getrandom(void *buf, uint64_t len, uint32_t flags) {
    (void) flags;
    if (!buf || !len) return 0;
    if (!uptr_ok_w(buf, len)) return -(int64_t) EFAULT;

    chacha20_rng_bytes(&g_chacha20_rng, (uint8_t *) buf, (size_t) len);
    return (int64_t) len;
}

#define AT_RESOLVED(ret, dfd, upath, out)                                                          \
    (((ret) = at_resolve((int) (dfd), (const char *) (upath), (out), sizeof(out))) >= 0)

static int64_t sys_prctl(int op, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void) op;
    (void) a2;
    (void) a3;
    (void) a4;
    (void) a5;
    return 0;
}

void syscall_dispatch(syscall_frame_t *f) {
    phantom_safe_point();
    uint64_t nr = f->rax;
    uint64_t a1 = f->rdi, a2 = f->rsi, a3 = f->rdx;
    uint64_t a4 = f->r10, a5 = f->r8, a6 = f->r9;

    proc_t *tp = cur();
    if (tp) tp->ptrace_orig_rax = nr;
    if (tp && tp->ptrace_syscall_trace && nr != 101) {
        tp->ptrace_in_syscall = 1;
        proc_ptrace_stop(tp, SIGTRAP | 0x80, 1, f, &f->r11);
    }

    int64_t ret;
    switch (nr) {
    case 0:
        ret = fd_read((int) a1, (void *) a2, a3);
        break;
    case 1:
        ret = fd_write((int) a1, (const void *) a2, a3);
        break;
    case 2:
        ret = fd_open((const char *) a1, (int) a2, (int) a3);
        break;
    case 3:
        ret = fd_close((int) a1);
        break;
    case 4:
        ret = fd_stat((const char *) a1, (struct linux_stat *) a2);
        break;
    case 5:
        ret = fd_fstat((int) a1, (struct linux_stat *) a2);
        break;
    case 6:
        ret = fd_lstat((const char *) a1, (struct linux_stat *) a2);
        break;
    case 7:
        ret = sys_poll((struct pollfd_s *) a1, a2, (int) a3);
        break;
    case 8:
        ret = fd_lseek((int) a1, (int64_t) a2, (int) a3);
        break;
    case 9:
        ret = sys_mmap(a1, a2, a3, a4, a5, a6);
        break;
    case 10:
        ret = sys_mprotect(a1, a2, a3);
        break;
    case 11:
        ret = sys_munmap(a1, a2);
        break;
    case 12:
        ret = sys_brk(a1);
        break;
    case 25:
        ret = sys_mremap(a1, a2, a3, a4, a5);
        break;
    case 26:
        ret = 0; /* noop ramfs*/
        break;
    case 27:
        if (a3) {
            uint64_t vec_len = (a2 + PAGE_SIZE - 1) / PAGE_SIZE;
            if (!uptr_ok_w((void *) a3, vec_len)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            memset((void *) a3, 1, vec_len);
        }
        ret = 0;
        break;
    case 13:
        ret = sys_rt_sigaction((int) a1, (const k_sigaction_t *) a2, (k_sigaction_t *) a3, a4);
        break;
    case 14:
        ret = sys_rt_sigprocmask((int) a1, (const uint64_t *) a2, (uint64_t *) a3, a4);
        break;
    case 15:
        ret = sys_rt_sigreturn(f);
        break;
    case 16:
        ret = fd_ioctl((int) a1, a2, a3);
        break;
    case 17:
        ret = fd_pread((int) a1, (void *) a2, a3, a4);
        break;
    case 18:
        ret = fd_pwrite((int) a1, (const void *) a2, a3, a4);
        break;
    case 19:
        ret = sys_readv((int) a1, (const struct iovec *) a2, (int) a3);
        break;
    case 20:
        ret = sys_writev((int) a1, (const void *) a2, (int) a3);
        break;
    case 21:
        ret = sys_access((const char *) a1, (int) a2);
        break;
    case 22:
        if (!a1 || !uptr_ok_w((void *) a1, 2 * sizeof(int))) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = fd_pipe((int *) a1);
        break;
    case 24: {
        ret = 0;
        break;
    }
    case 53:
        if (!a4 || !uptr_ok_w((void *) a4, 2 * sizeof(int))) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = fd_socketpair((int *) a4);
        break;
    case 23:
        ret = sys_select((int) a1, (void *) a2, (void *) a3, (void *) a4, (void *) a5);
        break;
    case 28:
        ret = sys_madvise((void *) a1, a2, (int) a3);
        break;
    case 29:
        ret = (int64_t) sys_shmget((int) a1, a2, (int) a3);
        break;
    case 30:
        ret = (int64_t) sys_shmat((int) a1, a2, (int) a3);
        break;
    case 31:
        ret = (int64_t) sys_shmctl((int) a1, (int) a2, (void *) a3);
        break;
    case 40:
        ret = sys_sendfile((int) a1, (int) a2, (uint64_t *) a3, a4);
        break;
    case 41: /* socket(domain, type, proto) */
        ret = (int64_t) fd_socket((int) a1, (int) a2, (int) a3);
        break;
    case 42: /* connect(fd, addr, addrlen) */
        ret = sys_socket_connect((int) a1, (struct sockaddr_un *) a2, a3);
        break;
    case 43: /* accept(fd, addr, addrlen) */
        ret = sys_socket_accept((int) a1, (struct sockaddr_un *) a2, (int *) a3, 0);
        break;
    case 44: { /* sendto(fd, buf, len, flags, addr, addrlen) */
        vfs_file_t *sf = fd_get_file((int) a1);
        if (sf && sf->inet) {
            if (a3 && !uptr_ok((void *) a2, a3)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            if (a5) {
                if (!uptr_ok((void *) a5, 16)) {
                    ret = -(int64_t) EFAULT;
                    break;
                }
                ret = inet_sendto(sf->inet, (const void *) a2, a3, (const struct sockaddr_in *) a5);
            } else {
                ret = inet_fd_write(sf->inet, (const void *) a2, a3);
            }
        } else {
            ret = fd_write((int) a1, (const void *) a2, a3);
        }
        break;
    }
    case 45: { /* recvfrom(fd, buf, len, flags, addr, addrlen_ptr) */
        vfs_file_t *sf = fd_get_file((int) a1);
        if (sf && sf->inet) {
            struct sockaddr_in *sa = a5 ? (struct sockaddr_in *) a5 : NULL;
            if (sa && !uptr_ok_w(sa, sizeof(*sa))) {
                ret = -(int64_t) EFAULT;
                break;
            }
            if (a3 && !uptr_ok_w((void *) a2, a3)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            int rflags = sf->flags | ((a4 & 0x40) ? O_NONBLOCK : 0); /* MSG_DONTWAIT */
            ret = inet_recvfrom(sf->inet, (void *) a2, a3, sa, rflags);
            if (ret >= 0 && sa && a6 && uptr_ok_w((void *) a6, sizeof(int)))
                *(int *) (uintptr_t) a6 = (int) sizeof(*sa);
        } else {
            ret = fd_read((int) a1, (void *) a2, a3);
        }
        break;
    }
    case 46: /* sendmsg(fd, msghdr, flags) */
        ret = sys_socket_sendmsg((int) a1, (const void *) a2, (int) a3);
        break;
    case 47: /* recvmsg(fd, msghdr, flags) */
        ret = sys_socket_recvmsg((int) a1, (void *) a2, (int) a3);
        break;
    case 48: { /* shutdown(fd, how) */
        vfs_file_t *sf = fd_get_file((int) a1);
        if (sf && sf->inet) {
            inet_conn_close(sf->inet);
            sf->inet = NULL;
        }
        ret = 0;
        break;
    }
    case 49: /* bind(fd, addr, addrlen) */
        ret = sys_socket_bind((int) a1, (struct sockaddr_un *) a2, a3);
        break;
    case 50: { /* listen(fd, backlog) */
        vfs_file_t *lf = fd_get_file((int) a1);
        if (lf && lf->inet)
            ret = inet_listen(lf->inet, (int) a2);
        else
            ret = (int64_t) fd_listen_unix((int) a1, (int) a2);
        break;
    }
    case 51: /* getsockname(fd, addr, addrlen) */
        ret = sys_socket_getsockname((int) a1, (struct sockaddr_un *) a2, (int *) a3);
        break;
    case 52: /* getpeername(fd, addr, addrlen) */
        ret = sys_socket_getpeername((int) a1, (struct sockaddr_un *) a2, (int *) a3);
        break;
    case 54:
        ret = sys_socket_setsockopt((int) a1, (int) a2, (int) a3, (void *) a4, (int) a5);
        break;
    case 55: /* getsockopt */
        ret = sys_socket_getsockopt((int) a1, (int) a2, (int) a3, (void *) a4, (int *) a5);
        break;
    case 32:
        ret = fd_dup((int) a1);
        break;
    case 33:
        ret = fd_dup2((int) a1, (int) a2);
        break;
    case 34:
        ret = sys_pause();
        break;
    case 35:
        ret = sys_nanosleep((void *) a1, (void *) a2);
        break;
    case 36:
        ret = sys_getitimer((int) a1, (void *) a2);
        break;
    case 37:
        ret = sys_alarm((uint32_t) a1);
        break;
    case 38:
        ret = sys_setitimer((int) a1, (const void *) a2, (void *) a3);
        break;
    case 39:
        ret = sys_getpid();
        break;
    case 56:
        ret = sys_clone(a1, a2, (uint32_t *) a3, (uint32_t *) a4, a5, f);
        break;
    case 57:
        ret = sys_fork(f);
        break;
    case 58:
        ret = sys_fork(f); /* vfork: treat as fork */
        break;
    case 59:
        ret = sys_execve((const char *) a1, (const char **) a2, (const char **) a3);
        break;
    case 60:
        proc_do_exit((int) a1);
        return;
    case 61:
        ret = sys_wait4((int) a1, (int *) a2, (int) a3, (void *) a4);
        break;
    case 62:
        ret = sys_kill(a1, (int) a2);
        break;
    case 63:
        ret = sys_uname((struct utsname *) a1);
        break;
    case 64:
    case 65:
    case 66:
    case 68:
    case 69:
    case 70:
    case 71:
        ret = -(int64_t) ENOSYS;
        break;
    case 67:
        ret = (int64_t) sys_shmdt(a1);
        break;
    case 72:
        ret = fd_fcntl((int) a1, (int) a2, a3);
        break;
    case 73:
        ret = fd_valid((int) a1) ? 0 : -(int64_t) EBADF;
        break;
    case 74:
    case 75:
        ret = fd_valid((int) a1) ? 0 : -(int64_t) EBADF;
        break;
    case 76:
        ret = sys_truncate((const char *) a1, a2);
        break;
    case 77:
        ret = sys_ftruncate((int) a1, a2);
        break;
    case 78:
        ret = fd_getdents64((int) a1, (void *) a2, a3);
        break;
    case 79:
        ret = sys_getcwd((char *) a1, a2);
        break;
    case 80:
        ret = sys_chdir((const char *) a1);
        break;
    case 81:
        ret = sys_fchdir((int) a1);
        break;
    case 82:
        ret = sys_rename((const char *) a1, (const char *) a2);
        break;
    case 83:
        ret = sys_mkdir((const char *) a1, (uint32_t) a2);
        break;
    case 84:
        ret = sys_rmdir((const char *) a1);
        break;
    case 85:
        ret = fd_open((const char *) a1, O_WRONLY | O_CREAT | O_TRUNC, (int) a2);
        break;
    case 86:
        ret = sys_link((const char *) a1, (const char *) a2);
        break;
    case 87:
        ret = sys_unlink((const char *) a1);
        break;
    case 88:
        ret = sys_symlink((const char *) a1, (const char *) a2);
        break;
    case 89:
        ret = fd_readlink((const char *) a1, (char *) a2, a3);
        break;
    case 90: {
        char abs[512];
        if (!path_abs(abs, (const char *) a1)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = (int64_t) vfs_chmod(abs, (uint32_t) a2);
        break;
    }
    case 91:
        ret = (int64_t) vfs_fchmod((int) a1, (uint32_t) a2);
        break;
    case 92: {
        char abs[512];
        if (!path_abs(abs, (const char *) a1)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = (int64_t) vfs_chown(abs, (uint32_t) a2, (uint32_t) a3);
        break;
    }
    case 93:
        ret = (int64_t) vfs_fchown((int) a1, (uint32_t) a2, (uint32_t) a3);
        break;
    case 94: {
        char abs[512];
        if (!path_abs(abs, (const char *) a1)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = (int64_t) vfs_lchown(abs, (uint32_t) a2, (uint32_t) a3);
        break;
    }
    case 95:
        ret = sys_umask(a1);
        break;
    case 96:
        ret = sys_gettimeofday((void *) a1, (void *) a2);
        break;
    case 97:
        ret = sys_getrlimit(a1, (void *) a2);
        break;
    case 98:
        if (a2) {
            if (!uptr_ok_w((void *) a2, 144)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            memset((void *) a2, 0, 144);
        }
        ret = 0;
        break;
    case 99:
        ret = sys_sysinfo((struct sysinfo_s *) a1);
        break;
    case 100:
        ret = sys_times((void *) a1);
        break;
    case 101:
        ret = sys_ptrace((int64_t) a1, (int64_t) a2, a3, a4);
        break;
    case 103:
        ret = -(int64_t) EPERM; /* syslog */
        break;
    case 102:
        ret = sys_getuid();
        break;
    case 104:
        ret = sys_getgid();
        break;
    case 105:
        ret = sys_setuid((uint32_t) a1);
        break;
    case 106:
        ret = sys_setgid((uint32_t) a1);
        break;
    case 107:
        ret = sys_geteuid();
        break;
    case 108:
        ret = sys_getegid();
        break;
    case 109:
        ret = sys_setpgid(a1, a2);
        break;
    case 110:
        ret = sys_getppid();
        break;
    case 111:
        ret = sys_getpgrp();
        break;
    case 112:
        ret = sys_setsid();
        break;
    case 113:
        ret = sys_setreuid((uint32_t) a1, (uint32_t) a2);
        break;
    case 114:
        ret = sys_setregid((uint32_t) a1, (uint32_t) a2);
        break;
    case 115:
        ret = sys_getgroups((int) a1, (uint32_t *) a2);
        break;
    case 116:
        ret = sys_setgroups((int) a1, (uint32_t *) a2);
        break;
    case 117:
        ret = sys_setresuid((uint32_t) a1, (uint32_t) a2, (uint32_t) a3);
        break;
    case 118:
        ret = sys_getresuid((uint32_t *) a1, (uint32_t *) a2, (uint32_t *) a3);
        break;
    case 119:
        ret = sys_setresgid((uint32_t) a1, (uint32_t) a2, (uint32_t) a3);
        break;
    case 120:
        ret = sys_getresgid((uint32_t *) a1, (uint32_t *) a2, (uint32_t *) a3);
        break;
    case 121:
        ret = sys_getpgid(a1);
        break;
    case 122:
        ret = sys_setfsuid((uint32_t) a1);
        break;
    case 123:
        ret = sys_setfsgid((uint32_t) a1);
        break;
    case 124: {
        proc_t *_p = a1 ? proc_find((uint32_t) a1) : cur();
        ret = _p ? (int64_t) _p->pgid : -(int64_t) ESRCH;
        if (a1 && _p) proc_unref(_p);
        break;
    }
    case 125:
        if (a2) {
            if (!uptr_ok_w((void *) a2, 40)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            memset((void *) a2, 0, 40);
        }
        ret = 0;
        break; /* capget: no caps */
    case 126:
        ret = -(int64_t) EPERM;
        break; /* capset */
    case 127: {
        if (a1) {
            if (!uptr_ok_w((void *) a1, 8)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            *(uint64_t *) a1 = cur() ? (cur()->pending_sigs & cur()->sig_mask) : 0;
        }
        ret = 0;
        break;
    }
    case 128:
        ret = sys_rt_sigtimedwait((const uint64_t *) a1, (void *) a2, (const void *) a3, a4);
        break;
    case 129:
        ret = 0;
        break; /* rt_sigqueueinfo */
    case 132:
        ret = 0;
        break; /* utime: no-op */
    case 130:
        ret = sys_rt_sigsuspend((const uint64_t *) a1, a2);
        break;
    case 131:
        ret = sys_sigaltstack((const void *) a1, (void *) a2);
        break;
    case 133: {
        char abs[512];
        if (!a1 || !path_abs(abs, (const char *) a1)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = (int64_t) vfs_mknod(abs, (uint32_t) a2, a3);
        break;
    }
    case 135:
        ret = 0;
        break; /* personality */
    case 139:
        ret = -(int64_t) ENOSYS;
        break; /* sysfs */
    case 140:
        ret = 0;
        break; /* getpriority */
    case 141:
        ret = is_root() ? 0 : -(int64_t) EPERM;
        break; /* setpriority */
    case 142:
        ret = is_root() ? 0 : -(int64_t) EPERM;
        break; /* sched_setparam */
    case 143:
        if (a2) {
            if (!uptr_ok_w((void *) a2, 4)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            ((int *) a2)[0] = 0;
        }
        ret = 0;
        break; /* sched_getparam */
    case 144:
        ret = is_root() ? 0 : -(int64_t) EPERM;
        break; /* sched_setscheduler */
    case 145:
        ret = 0;
        break; /* sched_getscheduler: SCHED_OTHER */
    case 146:
        ret = 0;
        break; /* sched_get_priority_max */
    case 147:
        ret = 0;
        break; /* sched_get_priority_min */
    case 148:
        if (a2) {
            if (!uptr_ok_w((void *) a2, 16)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            ((uint64_t *) a2)[0] = 0;
            ((uint64_t *) a2)[1] = 10000000ULL;
        }
        ret = 0;
        break; /* sched_rr_get_interval */
    case 149:
    case 150:
    case 151:
    case 152:
        ret = -(int64_t) ENOSYS;
        break; /* mlock/munlock */
    case 153:
        ret = host_priv() ? 0 : -(int64_t) EPERM;
        break; /* vhangup */
    case 137:
        ret = sys_statfs((const char *) a1, (void *) a2);
        break;
    case 138:
        ret = sys_statfs(NULL, (void *) a2);
        break;
    case 157:
        ret = sys_prctl((int) a1, a2, a3, a4, a5);
        break;
    case 158:
        ret = sys_arch_prctl((int) a1, a2);
        break;
    case 159:
        ret = host_priv() ? 0 : -(int64_t) EPERM;
        break; /* adjtimex */
    case 160:
        ret = sys_getrlimit(a1, (void *) a2); /* setrlimit: accept silently */
        break;
    case 161:
        ret = is_root() ? 0 : -(int64_t) EPERM;
        break; /* chroot: no-op until real roots exist */
    case 162:
        vfs_sync_all();
        ret = 0;
        break;
    case 169: { /* reboot(magic1, magic2, cmd, arg) */
#define REBOOT_MAGIC1 0xfee1deadu
#define REBOOT_CMD_RESTART 0x01234567u
#define REBOOT_CMD_HALT 0xcdef0123u
#define REBOOT_CMD_POWER_OFF 0x4321fedcu
        if (!host_priv()) {
            ret = -(int64_t) EPERM;
            break;
        }

        uint32_t cmd;
        if ((uint32_t) a1 == REBOOT_MAGIC1) {
            uint32_t m2 = (uint32_t) a2;
            if (m2 != 0x28121969u && m2 != 0x05121996u && m2 != 0x16041998u && m2 != 0x20112000u) {
                ret = -(int64_t) EINVAL;
                break;
            }
            cmd = (uint32_t) a3;
        } else {
            // legacy path
            cmd = (uint32_t) a1 ? (uint32_t) a1 : REBOOT_CMD_POWER_OFF;
        }

        switch (cmd) {
        case REBOOT_CMD_RESTART:
            vfs_sync_all();
            acpi_reboot();
            break;
        case REBOOT_CMD_POWER_OFF:
            vfs_sync_all();
            acpi_poweroff();
            break;
        case REBOOT_CMD_HALT:
            vfs_sync_all();
            cli();
            for (;;) hlt();
            break;
        default:
            ret = -(int64_t) EINVAL;
            break;
        }
        break;
    }
    case 164:
        ret = host_priv() ? 0 : -(int64_t) EPERM;
        break; /* settimeofday */
    case 165:  /* mount(source, target, fstype, flags, data) */
        ret = sys_mount((const char *) a1, (const char *) a2, (const char *) a3, a4, (void *) a5);
        break;
    case 166: /* umount2(target, flags) */
        ret = sys_umount2((const char *) a1, (int) a2);
        break;
    case 170:
        ret = host_priv() ? 0 : -(int64_t) EPERM; /* sethostname */
        break;
    case 171:
        ret = host_priv() ? 0 : -(int64_t) EPERM;
        break;  /* setdomainname */
    case 172: { /* iopl(level) - set io privilege level in RFLAGS */
        if (!host_priv()) {
            ret = -(int64_t) EPERM;
            break;
        }
        int level = (int) a1 & 3;
        f->r11 = (f->r11 & ~0x3000ULL) | ((uint64_t) level << 12);
        ret = 0;
        break;
    }
    case 173: { /* ioperm(from, count, turn_on) - we just grant iopl=3 for simplicity hahaha */
        if (!host_priv()) {
            ret = -(int64_t) EPERM;
            break;
        }
        (void) a1;
        (void) a2;
        if (a3) f->r11 |= 0x3000ULL; /* IOPL=3: allow all ports */
        ret = 0;
        break;
    }
    case 175:
    case 176:
        ret = -(int64_t) ENOSYS;
        break; /* init/delete_module */
    case 188:
    case 189:
    case 190:
    case 191:
    case 192:
    case 193:
    case 194:
    case 195:
    case 196:
    case 197:
    case 198:
    case 199:
        ret = -(int64_t) ENOTSUP; /* xattr: not supported */
        break;
    case 186:
        ret = sys_gettid();
        break;
    case 201: {
        uint64_t t = g_epoch_base + g_ticks / 1000;
        if (a1) {
            if (!uptr_ok_w((void *) a1, 8)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            *(uint64_t *) a1 = t;
        }
        ret = (int64_t) t;
        break;
    }
    case 203:
        ret = is_root() ? 0 : -(int64_t) EPERM;
        break; /* sched_setaffinity */
    case 204: {
        uint64_t sz = a2;
        if (a3 && sz > 0) {
            if (!uptr_ok_w((void *) a3, sz)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            memset((void *) a3, 0, sz);
            for (uint32_t i = 0; i < g_cpu_count && i < sz * 8; i++)
                ((uint8_t *) a3)[i / 8] |= (uint8_t) (1 << (i % 8));
        }
        ret = 0;
        break;
    } /* sched_getaffinity */
    case 213:
        ret = sys_epoll_create1(0);
        break; /* epoll_create */
    case 221:
        ret = sys_epoll_wait((int) a1, (struct epoll_event *) a2, (int) a3, -1);
        break; /* epoll_wait (old) */
    case 222:
    case 224:
    case 226:
        ret = 0;
        break; /* timer_create/gettime/delete stubs */
    case 223:
        ret = 0;
        break; /* timer_settime */
    case 225:
        ret = 0;
        break; /* timer_getoverrun */
    case 227:
        ret = host_priv() ? 0 : -(int64_t) EPERM;
        break; /* clock_settime */
    case 232:
        ret = sys_epoll_wait((int) a1, (struct epoll_event *) a2, (int) a3, (int) a4);
        break;
    case 233:
        ret = sys_epoll_ctl((int) a1, (int) a2, (int) a3, (struct epoll_event *) a4);
        break;
    case 200:
        ret = sys_kill((int64_t) a1, (int) a2);
        break; /* tkill */
    case 202:
        ret = sys_futex((uint32_t *) a1, (int) a2, (uint32_t) a3, (void *) a4, (uint32_t *) a5,
                        (uint32_t) a6);
        break;
    case 217:
        ret = fd_getdents64((int) a1, (void *) a2, a3);
        break;
    case 218:
        ret = sys_set_tid_address((void *) a1);
        break;
    case 228:
        ret = sys_clock_gettime(a1, (void *) a2);
        break;
    case 229:
        ret = sys_clock_getres(a1, (void *) a2);
        break;
    case 230:
        ret = sys_clock_nanosleep((int) a1, (int) a2, (const void *) a3, (void *) a4);
        break;
    case 231:
        proc_do_exit((int) a1);
        return;
    case 234:
        ret = sys_tgkill((int) a1, (int) a2, (int) a3);
        break;
    case 235:
        ret = 0; /* utimes */
        break;
    case 236:
        ret = -(int64_t) ENOSYS;
        break; /* vserver */
    case 240:
    case 241:
    case 242:
    case 243:
    case 244:
    case 245:
        ret = -(int64_t) ENOSYS;
        break; /* mqueue */
    case 251:
        ret = is_root() ? 0 : -(int64_t) EPERM;
        break; /* ioprio_set */
    case 252:
        ret = 0;
        break; /* ioprio_get */
    case 253:
    case 254:
    case 255:
        ret = -(int64_t) ENOSYS;
        break; /* inotify */
    case 247:  /* waitid */
        ret = sys_wait4((int) a2, NULL, (int) a4, NULL);
        break;
    case 257:
        ret = fd_openat((int) a1, (const char *) a2, (int) a3, (int) a4);
        break;
    case 258: {
        char abs[512];
        if (!AT_RESOLVED(ret, a1, a2, abs)) break;
        ret = (int64_t) vfs_mkdir(abs, (uint32_t) a3);
        break;
    }
    case 260: {
        char abs[512];
        if (!AT_RESOLVED(ret, a1, a2, abs)) break;
        ret = (int64_t) vfs_mknod(abs, (uint32_t) a3, a4);
        break;
    } /* mknodat */
    case 261: {
        char abs[512];
        if (!AT_RESOLVED(ret, a1, a2, abs)) break;
        int _r = (int) a5 & AT_SYMLINK_NOFOLLOW ?
                     (int) vfs_lchown(abs, (uint32_t) a3, (uint32_t) a4) :
                     (int) vfs_chown(abs, (uint32_t) a3, (uint32_t) a4);
        ret = _r;
        break;
    } /* fchownat */
    case 262:
        ret = fd_fstatat((int) a1, (const char *) a2, (struct linux_stat *) a3, (int) a4);
        break;
    case 263: {
        char abs[512];
        if (!AT_RESOLVED(ret, a1, a2, abs)) break;
        ret = ((int) a3 & 0x200) ? (int64_t) vfs_rmdir(abs) : (int64_t) vfs_unlink(abs);
        break;
    }
    case 264: {
        char ao[512], an[512];
        if (!AT_RESOLVED(ret, a1, a2, ao)) break;
        if (!AT_RESOLVED(ret, a3, a4, an)) break;
        ret = (int64_t) vfs_rename(ao, an);
        break;
    }
    case 265: {
        char ao[512], an[512];
        if (!AT_RESOLVED(ret, a1, a2, ao)) break;
        if (!AT_RESOLVED(ret, a3, a4, an)) break;
        ret = (int64_t) vfs_link(ao, an);
        break;
    }
    case 266: {
        char abs[512], tgt[512];
        if (!copy_user_path(tgt, (const char *) a1)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        if (!AT_RESOLVED(ret, a2, a3, abs)) break;
        ret = vfs_create_symlink(abs, tgt) ? 0 : -(int64_t) EEXIST;
        break;
    }
    case 267: {
        char abs[512];
        if (!AT_RESOLVED(ret, a1, a2, abs)) break;
        ret = (int64_t) fd_readlink_kpath(abs, (char *) a3, a4);
        break;
    }
    case 268: {
        char abs[512];
        if (!AT_RESOLVED(ret, a1, a2, abs)) break;
        ret = (int64_t) vfs_chmod(abs, (uint32_t) a3);
        break;
    }
    case 269: {
        char abs[512];
        if (!AT_RESOLVED(ret, a1, a2, abs)) break;
        ret = (int64_t) vfs_access(abs, (int) a3);
        break;
    } /* faccessat */
    case 270:
        ret =
            sys_pselect6((int) a1, (void *) a2, (void *) a3, (void *) a4, (void *) a5, (void *) a6);
        break;
    case 271:
        ret = sys_ppoll((struct pollfd_s *) a1, a2, (void *) a3, (const void *) a4, a5);
        break;
    case 272:
        ret = -(int64_t) ENOSYS;
        break; /* unshare */
    case 273:
        ret = sys_set_robust_list((void *) a1, a2);
        break;
    case 274:
    case 275:
    case 276:
    case 277:
        ret = -(int64_t) ENOSYS;
        break; /* splice/tee/sync_file_range/vmsplice */
    case 278:
        ret = -(int64_t) ENOSYS;
        break; /* move_pages */
    case 279:
        ret = 0; /* utimensat */
        break;
    case 280:
        ret = fd_openat((int) a1, (const char *) a2, (int) a3, (int) a4);
        break; /* openat2 */
    case 281:
        ret = sys_epoll_wait((int) a1, (struct epoll_event *) a2, (int) a3, (int) a4);
        break; /* epoll_pwait */
    case 282:
        ret = -(int64_t) ENOSYS;
        break; /* signalfd: not implemented */
    case 283:
        ret = fd_timerfd_create((int) a1, (int) a2);
        break; /* timerfd_create */
    case 284:
        ret = fd_eventfd((uint32_t) a1, (int) a2);
        break; /* eventfd */
    case 285:
        ret = 0;
        break; /* fallocate: no-op */
    case 286:  /* timerfd_settime(fd, flags, new, old) */
        if (!a3 || !uptr_ok((void *) a3, 32)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        if (a4 && !uptr_ok_w((void *) a4, 32)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = fd_timerfd_settime((int) a1, (int) a2, (const kitimerspec_t *) a3,
                                 (kitimerspec_t *) a4);
        break;
    case 287: /* timerfd_gettime(fd, curr) */
        if (!a2 || !uptr_ok_w((void *) a2, 32)) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = fd_timerfd_gettime((int) a1, (kitimerspec_t *) a2);
        break;
    case 288: /* accept4(fd, addr, addrlen, flags) */
        ret = sys_socket_accept((int) a1, (struct sockaddr_un *) a2, (int *) a3, (int) a4);
        break;
    case 289:
        ret = -(int64_t) ENOSYS;
        break; /* signalfd4: not implemented */
    case 290:
        ret = fd_eventfd((uint32_t) a1, (int) a2);
        break; /* eventfd2 */
    case 291:
        ret = sys_epoll_create1((int) a1);
        break; /* epoll_create1 */
    case 292:
        ret = fd_dup3((int) a1, (int) a2, (int) a3);
        break;
    case 293: /* pipe2(fds, flags) */
        if (!a1 || !uptr_ok_w((void *) a1, 2 * sizeof(int))) {
            ret = -(int64_t) EFAULT;
            break;
        }
        ret = fd_pipe((int *) a1);
        if (ret == 0 && (a2 & (O_CLOEXEC | O_NONBLOCK))) {
            int *pf = (int *) a1;
            for (int _e = 0; _e < 2; _e++) {
                vfs_file_t *pe = fd_get_file(pf[_e]);
                if (!pe) continue;
                if (a2 & O_CLOEXEC) pe->cloexec = 1;
                if (a2 & O_NONBLOCK) pe->flags |= O_NONBLOCK;
            }
        }
        break;
    case 294:
        ret = -(int64_t) ENOSYS;
        break; /* inotify_init1 */
    case 295:
        ret = sys_preadv((int) a1, (const struct iovec *) a2, (int) a3, a4);
        break; /* preadv */
    case 296:
        ret = sys_pwritev((int) a1, (const struct iovec *) a2, (int) a3, a4);
        break; /* pwritev */
    case 300:
        ret = host_priv() ? 0 : -(int64_t) EPERM;
        break; /* clock_adjtime */
    case 301:
        ret = 0;
        break; /* syncfs: no-op */
    case 302:
        ret = sys_prlimit64(a1, a2, (void *) a3, (void *) a4);
        break;
    case 303:
        ret = -(int64_t) ENOSYS;
        break; /* finit_module */
    case 304:
        ret = is_root() ? 0 : -(int64_t) EPERM;
        break; /* sched_setattr */
    case 305:
        if (a2) {
            if (!uptr_ok_w((void *) a2, 56)) {
                ret = -(int64_t) EFAULT;
                break;
            }
            memset((void *) a2, 0, 56);
        }
        ret = 0;
        break; /* sched_getattr */
    case 306: {
        char ao[512], an[512];
        if (!AT_RESOLVED(ret, a1, a2, ao)) break;
        if (!AT_RESOLVED(ret, a3, a4, an)) break;
        ret = (int64_t) vfs_rename(ao, an);
        break;
    } /* renameat2 */
    case 307:
        ret = -(int64_t) ENOSYS;
        break; /* seccomp not implemented */
    case 318:
        ret = sys_getrandom((void *) a1, a2, (uint32_t) a3);
        break;
    case 319:
        ret = sys_memfd_create((const char *) a1, (uint32_t) a2);
        break;
    case 324:
        ret = 0;
        break;
    case 325:
        ret = -(int64_t) ENOSYS;
        break; /* mlock2 */
    case 326:
        ret = sys_copy_file_range((int) a1, (uint64_t *) a2, (int) a3, (uint64_t *) a4, a5,
                                  (uint32_t) a6);
        break;
    case 327:
        ret = sys_preadv((int) a1, (const struct iovec *) a2, (int) a3, a4);
        break; /* preadv2 */
    case 328:
        ret = sys_pwritev((int) a1, (const struct iovec *) a2, (int) a3, a4);
        break; /* pwritev2 */
    case 332:
        ret = sys_statx((int) a1, (const char *) a2, (int) a3, (uint32_t) a4, (struct statx *) a5);
        break;
    case 334: {
        int _lo = (int) a1, _hi = (int) a2;
        if (_lo < 0) _lo = 0;
        if (_hi >= VFS_FD_MAX) _hi = VFS_FD_MAX - 1;
        for (int _fd = _lo; _fd <= _hi; _fd++)
            if (fd_valid(_fd)) fd_close(_fd);
        ret = 0;
        break;
    } /* close_range */
    case 439: {
        char abs[512];
        if (!AT_RESOLVED(ret, a1, a2, abs)) break;
        ret = (int64_t) vfs_access(abs, (int) a3);
        break;
    } /* faccessat2 */

    case SYS_jail_create:
        ret = sys_jail_create((const kjail_conf_t *) a1);
        break;
    case SYS_jail_attach:
        ret = sys_jail_attach((uint32_t) a1);
        break;
    case SYS_jail_get:
        ret = sys_jail_get((uint32_t) a1, (kjail_info_t *) a2);
        break;
    case SYS_jail_list:
        ret = sys_jail_list((uint32_t *) a1, (int) a2);
        break;
    case SYS_jail_remove:
        ret = sys_jail_remove((uint32_t) a1);
        break;
    case SYS_jail_self:
        ret = sys_jail_self();
        break;
    case SYS_jail_set_auto:
        ret = sys_jail_set_auto((int) a1);
        break;
    case SYS_phantom_mode:
        ret = sys_phantom_mode((int) a1);
        break;
    case SYS_phantom_read:
        ret = sys_phantom_read((void *) a1, (uint32_t) a2);
        break;

    default:
        log_debug("[syscall %lu  a1=%lx a2=%lx a3=%lx]", nr, a1, a2, a3);
        ret = -(int64_t) ENOSYS;
        break;
    }
    f->rax = (uint64_t) ret;

    if (tp && tp->ptrace_syscall_trace && nr != 101) {
        tp->ptrace_in_syscall = 0;
        proc_ptrace_stop(tp, SIGTRAP | 0x80, 1, f, &f->r11);
    }

    signal_check(f);
}
