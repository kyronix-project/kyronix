#include "procfs.h"
#include "arch/x86_64/pit.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/proc.h"
#include "syscall/syscall.h"

#define EINVAL 22
#define ENOENT 2
#define EPERM 1
#define EFAULT 14

static int64_t read_buf(char* out, uint64_t len, uint64_t off, const char* src, uint64_t sz)
{
    if (!uptr_ok_w(out, len))
        return -(int64_t) EFAULT;
    if (off >= sz)
        return 0;
    uint64_t r = sz - off;
    if (r > len)
        r = len;
    memcpy(out, src + off, r);
    return (int64_t) r;
}

static const char* proc_name(proc_t* p)
{
    if (!p || !p->exe_path[0])
        return "unknown";
    const char* s = p->exe_path;
    for (const char* c = p->exe_path; *c; c++)
        if (*c == '/')
            s = c + 1;
    return *s ? s : p->exe_path;
}

static char proc_state_char(proc_t* p)
{
    if (!p)
        return 'R';
    switch (p->state) {
    case PROC_RUNNING:
        return 'R';
    case PROC_READY:
        return 'R';
    case PROC_WAITING:
        return 'S';
    case PROC_ZOMBIE:
        return 'Z';
    default:
        return 'I';
    }
}

static int proc_count(int state)
{
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proctable[i].state == state)
            n++;
    return n;
}

static int proc_alive_count(void)
{
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proctable[i].state != PROC_UNUSED)
            n++;
    return n;
}

static int proc_last_pid(void)
{
    int last = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proctable[i].state != PROC_UNUSED && (int) g_proctable[i].pid > last)
            last = (int) g_proctable[i].pid;
    return last;
}

static int64_t proc_version_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    static const char ver[] = "Kyronix version 0.1 (x86_64)\n";
    return read_buf(buf, len, off, ver, sizeof(ver) - 1);
}

static int64_t proc_cpuinfo_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    char tmp[512];
    int sz = snprintf(tmp, sizeof(tmp),
                      "processor\t: 0\n"
                      "vendor_id\t: Kyronix\n"
                      "cpu family\t: 6\n"
                      "model\t\t: 0\n"
                      "model name\t: Kyronix virtual x86_64 CPU\n"
                      "stepping\t: 0\n"
                      "cpu MHz\t\t: 1000.000\n"
                      "cache size\t: 0 KB\n"
                      "physical id\t: 0\n"
                      "siblings\t: 1\n"
                      "core id\t\t: 0\n"
                      "cpu cores\t: 1\n"
                      "apicid\t\t: 0\n"
                      "flags\t\t: fpu tsc msr pae cx8 apic sep mtrr pge cmov pat mmx fxsr sse sse2 "
                      "syscall nx lm\n\n");
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_meminfo_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    uint64_t total = pmm_total_pages() * (PAGE_SIZE / 1024);
    uint64_t free = pmm_free_pages() * (PAGE_SIZE / 1024);
    uint64_t used = total > free ? total - free : 0;
    char tmp[512];
    int sz = snprintf(tmp, sizeof(tmp),
                      "MemTotal:       %lu kB\n"
                      "MemFree:        %lu kB\n"
                      "MemAvailable:   %lu kB\n"
                      "Buffers:        0 kB\n"
                      "Cached:         0 kB\n"
                      "SwapCached:     0 kB\n"
                      "Active:         %lu kB\n"
                      "Inactive:       0 kB\n"
                      "SwapTotal:      0 kB\n"
                      "SwapFree:       0 kB\n",
                      total, free, free, used);
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_uptime_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    uint64_t ms = g_ticks;
    char tmp[64];
    int sz = snprintf(tmp, sizeof(tmp), "%lu.%02lu %lu.%02lu\n", ms / 1000, (ms / 10) % 100,
                      ms / 1000, (ms / 10) % 100);
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_loadavg_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    char tmp[96];
    int runnable = proc_count(PROC_READY) + proc_count(PROC_RUNNING);
    int alive = proc_alive_count();
    int sz =
        snprintf(tmp, sizeof(tmp), "0.00 0.00 0.00 %d/%d %d\n", runnable, alive, proc_last_pid());
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_stat_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    uint64_t ticks = g_ticks / 10;
    char tmp[512];
    int sz =
        snprintf(tmp, sizeof(tmp),
                 "cpu  %lu 0 %lu %lu 0 0 0 0 0 0\n"
                 "cpu0 %lu 0 %lu %lu 0 0 0 0 0 0\n"
                 "intr 0\n"
                 "ctxt 0\n"
                 "btime %lu\n"
                 "processes %d\n"
                 "procs_running %d\n"
                 "procs_blocked %d\n",
                 ticks, ticks / 4, ticks, ticks, ticks / 4, ticks, g_epoch_base, proc_last_pid(),
                 proc_count(PROC_READY) + proc_count(PROC_RUNNING), proc_count(PROC_WAITING));
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_mounts_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    static const char mounts[] = "ramfs / ramfs rw 0 0\n"
                                 "proc /proc proc rw,nosuid,nodev,noexec 0 0\n"
                                 "sysfs /sys sysfs rw,nosuid,nodev,noexec 0 0\n"
                                 "devtmpfs /dev devtmpfs rw,nosuid 0 0\n";
    return read_buf(buf, len, off, mounts, sizeof(mounts) - 1);
}

static int64_t proc_filesystems_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    static const char filesystems[] = "nodev\tproc\n"
                                      "nodev\tsysfs\n"
                                      "nodev\tdevtmpfs\n"
                                      "\tramfs\n";
    return read_buf(buf, len, off, filesystems, sizeof(filesystems) - 1);
}

static int64_t proc_devices_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    static const char devices[] = "Character devices:\n"
                                  "  1 mem\n"
                                  "  4 tty\n"
                                  "  5 /dev/tty\n"
                                  " 10 misc\n"
                                  " 13 input\n"
                                  " 29 fb\n\n"
                                  "Block devices:\n";
    return read_buf(buf, len, off, devices, sizeof(devices) - 1);
}

static int64_t proc_kmsg_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    (void) buf;
    (void) len;
    (void) off;
    return 0;
}

static int64_t proc_ostype_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    static const char s[] = "Kyronix\n";
    return read_buf(buf, len, off, s, sizeof(s) - 1);
}

static int64_t proc_osrelease_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    static const char s[] = "0.0.1\n";
    return read_buf(buf, len, off, s, sizeof(s) - 1);
}

static int64_t proc_hostname_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    static const char s[] = "kx\n";
    return read_buf(buf, len, off, s, sizeof(s) - 1);
}

static int64_t proc_self_exe_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    proc_t* p = g_current_proc;
    if (!p || !p->exe_path[0])
        return 0;
    return read_buf(buf, len, off, p->exe_path, strlen(p->exe_path));
}

static int64_t proc_self_cmdline_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    proc_t* p = g_current_proc;
    if (!p || !p->exe_path[0])
        return 0;
    return read_buf(buf, len, off, p->exe_path, strlen(p->exe_path) + 1);
}

static int64_t proc_self_environ_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    (void) buf;
    (void) len;
    (void) off;
    return 0;
}

static int64_t proc_self_status_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    proc_t* p = g_current_proc;
    if (!p)
        return 0;
    char tmp[1024];
    int threads = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proctable[i].state != PROC_UNUSED && g_proctable[i].space == p->space)
            threads++;
    int sz = snprintf(tmp, sizeof(tmp),
                      "Name:\t%s\n"
                      "State:\t%c\n"
                      "Tgid:\t%u\n"
                      "Pid:\t%u\n"
                      "PPid:\t%u\n"
                      "Uid:\t%u\t%u\t%u\t%u\n"
                      "Gid:\t%u\t%u\t%u\t%u\n"
                      "FDSize:\t%d\n"
                      "Threads:\t%d\n"
                      "SigPnd:\t%016lx\n"
                      "SigBlk:\t%016lx\n"
                      "VmPeak:\t0 kB\n"
                      "VmSize:\t0 kB\n",
                      proc_name(p), proc_state_char(p), p->pid, p->pid, p->ppid, p->uid, p->euid,
                      p->suid, p->fsuid, p->gid, p->egid, p->sgid, p->fsgid, VFS_FD_MAX,
                      threads ? threads : 1, p->pending_sigs, p->sig_mask);
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_self_stat_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    proc_t* p = g_current_proc;
    if (!p)
        return 0;
    char tmp[512];
    int sz = snprintf(tmp, sizeof(tmp),
                      "%u (%s) %c %u %d 0 0 0 0 0 0 0 0 0 0 0 20 0 1 0 %lu 0 0 0 0 0 0 0 0 0 0 0 0 "
                      "0 0 0 0 0 0 0\n",
                      p->pid, proc_name(p), proc_state_char(p), p->ppid, p->pgid, g_ticks / 10);
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_self_maps_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    proc_t* p = g_current_proc;
    if (!p)
        return 0;
    char tmp[768];
    const char* exe = p->exe_path[0] ? p->exe_path : "";
    /* randomized addresses: don't leak actual VA to non-root */
    int sz = snprintf(tmp, sizeof(tmp),
                      "00400000-00800000 r-xp 00000000 00:00 0 %s\n"
                      "xxxxxxxx-xxxxxxxx rw-p 00000000 00:00 0 [heap]\n"
                      "yyyyyyyy-yyyyyyyy rw-p 00000000 00:00 0 [stack]\n",
                      exe);
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_self_pagemap_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    proc_t* p = g_current_proc;
    if (p && p->euid != 0)
        return -(int64_t) EPERM;
    if (!p || !p->space || len < 8)
        return 0;
    uint64_t nentries = len / 8;
    uint64_t written = 0;
    for (uint64_t i = 0; i < nentries; i++) {
        uint64_t va = ((off / 8) + i) << 12;
        uint64_t phys = vmm_virt_to_phys(p->space, va);
        uint64_t entry = phys ? ((phys >> 12) | (1ULL << 63)) : 0;
        memcpy(buf + i * 8, &entry, 8);
        written += 8;
    }
    return (int64_t) written;
}

static uint16_t dirent_record_len(const char* name)
{
    return (uint16_t) ((sizeof(struct linux_dirent64) + strlen(name) + 1 + 7) & ~7U);
}

static bool emit_dirent(uint8_t* out, uint64_t count, uint64_t* done, uint64_t ino, int64_t off,
                        uint8_t type, const char* name)
{
    uint16_t rec = dirent_record_len(name);
    if (*done + rec > count)
        return false;
    struct linux_dirent64* d = (struct linux_dirent64*) (out + *done);
    uint64_t namelen = strlen(name) + 1;
    if (*done + sizeof(*d) + namelen > count)
        return false;
    d->d_ino = ino;
    d->d_off = off;
    d->d_reclen = rec;
    d->d_type = type;
    memcpy(d->d_name, name, namelen);
    *done += rec;
    return true;
}

bool procfs_getdents64(vfs_node_t* dir, uint64_t* pos, void* buf, uint64_t count, int* out)
{
    char path[512];
    vfs_node_abspath(dir, path, sizeof(path));
    if (strcmp(path, "/proc/self/fd") != 0 && strcmp(path, "/dev/fd") != 0)
        return false;

    uint8_t* dst = (uint8_t*) buf;
    uint64_t done = 0;
    uint64_t emitted = 0;
    uint64_t idx = 0;
    uint64_t skip = *pos;

    if (idx++ >= skip && emit_dirent(dst, count, &done, dir->ino, (int64_t) idx, DT_DIR, "."))
        emitted++;
    if (idx++ >= skip && emit_dirent(dst, count, &done, dir->parent ? dir->parent->ino : dir->ino,
                                     (int64_t) idx, DT_DIR, ".."))
        emitted++;

    vfs_file_t** fds = vfs_get_fdtable();
    for (int fd = 0; fd < VFS_FD_MAX; fd++) {
        if (!fds || !fds[fd])
            continue;
        char name[16];
        snprintf(name, sizeof(name), "%d", fd);
        if (idx++ < skip)
            continue;
        if (!emit_dirent(dst, count, &done, 100000u + (uint64_t) fd, (int64_t) idx, DT_LNK, name))
            break;
        emitted++;
    }

    *pos += emitted;
    *out = (int) done;
    return true;
}

static int parse_fd_link(const char* path)
{
    const char* s = NULL;
    if (strncmp(path, "/proc/self/fd/", 14) == 0)
        s = path + 14;
    else if (strncmp(path, "/dev/fd/", 8) == 0)
        s = path + 8;
    else
        return -1;
    if (!*s)
        return -1;
    int fd = 0;
    while (*s) {
        if (*s < '0' || *s > '9')
            return -1;
        if (fd >= 102400) /* prevent overflow - max fd is 1024 */
            return -1;
        fd = fd * 10 + (*s - '0');
        s++;
    }
    return fd;
}

bool procfs_readlink(const char* path, char* buf, uint64_t bufsz, int* out)
{
    if (!uptr_ok_w(buf, bufsz)) {
        *out = -(int) EFAULT;
        return true;
    }

    if (strcmp(path, "/proc/self/exe") == 0) {
        proc_t* p = g_current_proc;
        if (!p || !p->exe_path[0]) {
            *out = -(int) ENOENT;
            return true;
        }
        uint64_t n = strlen(p->exe_path);
        if (n > bufsz)
            n = bufsz;
        memcpy(buf, p->exe_path, n);
        *out = (int) n;
        return true;
    }

    int fd = parse_fd_link(path);
    if (fd < 0)
        return false;
    vfs_file_t* f = fd_get_file(fd);
    if (!f) {
        *out = -(int) ENOENT;
        return true;
    }

    char tmp[512];
    if (f->node)
        vfs_node_abspath(f->node, tmp, sizeof(tmp));
    else if (f->wpipe)
        snprintf(tmp, sizeof(tmp), "socket:[%d]", fd);
    else if (f->pipe)
        snprintf(tmp, sizeof(tmp), "pipe:[%d]", fd);
    else
        snprintf(tmp, sizeof(tmp), "anon_inode:[%d]", fd);

    uint64_t n = strlen(tmp);
    if (n > bufsz)
        n = bufsz;
    memcpy(buf, tmp, n);
    *out = (int) n;
    return true;
}

void procfs_init(void)
{
    vfs_mkdir_p("/proc", 0555);
    vfs_mkdir_p("/proc/self", 0555);
    vfs_mkdir_p("/proc/self/fd", 0500);
    vfs_mkdir_p("/proc/bus/pci", 0555);
    vfs_mkdir_p("/proc/bus/input", 0555);
    vfs_mkdir_p("/proc/sys/kernel", 0555);

    vfs_create_chr("/proc/version", proc_version_read, NULL);
    vfs_create_chr("/proc/kmsg", proc_kmsg_read, NULL);
    vfs_create_chr("/proc/cpuinfo", proc_cpuinfo_read, NULL);
    vfs_create_chr("/proc/meminfo", proc_meminfo_read, NULL);
    vfs_create_chr("/proc/uptime", proc_uptime_read, NULL);
    vfs_create_chr("/proc/loadavg", proc_loadavg_read, NULL);
    vfs_create_chr("/proc/stat", proc_stat_read, NULL);
    vfs_create_chr("/proc/mounts", proc_mounts_read, NULL);
    vfs_create_symlink("/proc/self/mounts", "/proc/mounts");
    vfs_create_chr("/proc/filesystems", proc_filesystems_read, NULL);
    vfs_create_chr("/proc/devices", proc_devices_read, NULL);
    vfs_create_chr("/proc/sys/kernel/ostype", proc_ostype_read, NULL);
    vfs_create_chr("/proc/sys/kernel/osrelease", proc_osrelease_read, NULL);
    vfs_create_chr("/proc/sys/kernel/hostname", proc_hostname_read, NULL);

    vfs_create_chr("/proc/self/exe", proc_self_exe_read, NULL);
    vfs_create_chr("/proc/self/cmdline", proc_self_cmdline_read, NULL);
    vfs_create_chr("/proc/self/environ", proc_self_environ_read, NULL);
    vfs_create_chr("/proc/self/status", proc_self_status_read, NULL);
    vfs_create_chr("/proc/self/stat", proc_self_stat_read, NULL);
    vfs_create_chr("/proc/self/maps", proc_self_maps_read, NULL);

    vfs_node_t* pm = vfs_create_chr("/proc/self/pagemap", proc_self_pagemap_read, NULL);
    if (pm)
        pm->mode = S_IFCHR | 0400;
}
