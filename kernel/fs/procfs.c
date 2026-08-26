#include "procfs.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/pit.h"
#include "drivers/pci.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "version.h"
#ifdef CONFIG_KMEMLEAK
#include "mm/kmemleak.h"
#endif
#ifdef CONFIG_PROFILER
#include "prof/profiler.h"
#endif
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "module/loader.h"
#include "proc/jail.h"
#include "proc/proc.h"
#include "syscall/fsops.h"
#include "syscall/mount.h"
#include "syscall/syscall.h"

#define EINVAL 22
#define ENOENT 2
#define EPERM 1
#define EFAULT 14
#define ENOMEM 12

static int64_t read_buf(char *out, uint64_t len, uint64_t off, const char *src, uint64_t sz) {
    if (off >= sz) return 0;
    uint64_t r = sz - off;
    if (r > len) r = len;
    if (!uptr_ok_w(out, r)) return -(int64_t) EFAULT;
    memcpy(out, src + off, r);
    return (int64_t) r;
}

static const char *proc_name(proc_t *p) {
    if (!p || !p->exe_path[0]) return "unknown";
    const char *s = p->exe_path;
    for (const char *c = p->exe_path; *c; c++)
        if (*c == '/') s = c + 1;
    return *s ? s : p->exe_path;
}

static char proc_state_char(proc_t *p) {
    if (!p) return 'R';
    if (p->job_stopped) return 'T';
    switch (p->state) {
    case PROC_RUNNING:
        return 'R';
    case PROC_READY:
        return 'R';
    case PROC_WAITING:
        return 'S';
    case PROC_ZOMBIE:
        return 'Z';
    case PROC_STOPPED:
        return 'T';
    default:
        return 'I';
    }
}

static int proc_count(int state) {
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proctable[i].state == state && jail_can_see(g_current_proc, &g_proctable[i])) n++;
    return n;
}

static int proc_alive_count(void) {
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proctable[i].state != PROC_UNUSED && jail_can_see(g_current_proc, &g_proctable[i]))
            n++;
    return n;
}

static int proc_last_pid(void) {
    int last = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proctable[i].state != PROC_UNUSED && jail_can_see(g_current_proc, &g_proctable[i]) &&
            (int) g_proctable[i].pid > last)
            last = (int) g_proctable[i].pid;
    return last;
}

static int64_t proc_version_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    static const char ver[] = "Kyronix version " KERNEL_VERSION " (x86_64)\n";
    return read_buf(buf, len, off, ver, sizeof(ver) - 1);
}

static int64_t proc_pci_devices_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    char data[8192];
    size_t used = 0;
    for (int i = 0; i < g_pci_ndevs && used < sizeof(data); i++) {
        pci_dev_t *d = &g_pci_devs[i];
        int written = snprintf(data + used, sizeof(data) - used,
                               "%02x:%02x.%x %04x:%04x class %02x:%02x prog-if %02x irq %u\n",
                               d->bus, d->dev, d->fn, d->vendor, d->device,
                               d->class, d->subclass, d->prog_if, d->irq_line);
        if (written < 0) break;
        if ((size_t)written >= sizeof(data) - used) {
            used = sizeof(data);
            break;
        }
        used += (size_t)written;
    }
    return read_buf(buf, len, off, data, used);
}

static int64_t proc_cpuinfo_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;

    uint32_t eax, ebx, ecx, edx;

    cpuid(0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;
    char vendor[13];
    memcpy(vendor, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';

    cpuid(1, &eax, &ebx, &ecx, &edx);
    uint32_t stepping = eax & 0xf;
    uint32_t model = (eax >> 4) & 0xf;
    uint32_t family = (eax >> 8) & 0xf;
    uint32_t ext_model = (eax >> 16) & 0xf;
    uint32_t ext_family = (eax >> 20) & 0xff;
    uint32_t apicid = (ebx >> 24) & 0xff;
    uint32_t feat_ecx = ecx;
    uint32_t feat_edx = edx;

    if (family == 15) family = ext_family + family;
    if (family == 6 || family == 15) model = (ext_model << 4) | model;

    char brand[49] = { 0 };
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000004) {
        uint32_t *b = (uint32_t *) brand;
        cpuid(0x80000002, &b[0], &b[1], &b[2], &b[3]);
        cpuid(0x80000003, &b[4], &b[5], &b[6], &b[7]);
        cpuid(0x80000004, &b[8], &b[9], &b[10], &b[11]);
        brand[48] = '\0';
        char *p = brand;
        while (*p == ' ') p++;
        if (p != brand) memmove(brand, p, strlen(p) + 1);
        p = brand + strlen(brand);
        while (p > brand && p[-1] == ' ') p--;
        *p = '\0';
    }
    if (!brand[0]) strcpy(brand, "Unknown");

    uint32_t ext_edx = 0;
    if (max_leaf >= 0x80000001) {
        cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
        ext_edx = edx;
    }

    char flags[512];
    int fpos = 0;

#define ADD_FLAG(cond, name)                                                                       \
    do {                                                                                           \
        if (cond) {                                                                                \
            if (fpos > 0) flags[fpos++] = ' ';                                                     \
            int _n = snprintf(flags + fpos, (int) (sizeof(flags) - (uint64_t) fpos), "%s", name);  \
            if (_n > 0) fpos += _n;                                                                \
        }                                                                                          \
    } while (0)

    ADD_FLAG(feat_edx & (1 << 0), "fpu");
    ADD_FLAG(feat_edx & (1 << 4), "tsc");
    ADD_FLAG(feat_edx & (1 << 5), "msr");
    ADD_FLAG(feat_edx & (1 << 6), "pae");
    ADD_FLAG(feat_edx & (1 << 8), "cx8");
    ADD_FLAG(feat_edx & (1 << 9), "apic");
    ADD_FLAG(feat_edx & (1 << 11), "sep");
    ADD_FLAG(feat_edx & (1 << 12), "mtrr");
    ADD_FLAG(feat_edx & (1 << 13), "pge");
    ADD_FLAG(feat_edx & (1 << 15), "cmov");
    ADD_FLAG(feat_edx & (1 << 16), "pat");
    ADD_FLAG(feat_edx & (1 << 23), "mmx");
    ADD_FLAG(feat_edx & (1 << 24), "fxsr");
    ADD_FLAG(feat_edx & (1 << 25), "sse");
    ADD_FLAG(feat_edx & (1 << 26), "sse2");
    ADD_FLAG(feat_ecx & (1 << 0), "sse3");
    ADD_FLAG(feat_ecx & (1 << 9), "ssse3");
    ADD_FLAG(feat_ecx & (1 << 12), "fma");
    ADD_FLAG(feat_ecx & (1 << 19), "sse4_1");
    ADD_FLAG(feat_ecx & (1 << 20), "sse4_2");
    ADD_FLAG(feat_ecx & (1 << 23), "popcnt");
    ADD_FLAG(feat_ecx & (1 << 25), "aes");
    ADD_FLAG(feat_ecx & (1 << 28), "avx");
    ADD_FLAG(feat_ecx & (1 << 29), "f16c");
    ADD_FLAG(feat_ecx & (1 << 30), "rdrand");
    ADD_FLAG(feat_ecx & (1 << 31), "hypervisor");
    ADD_FLAG(ext_edx & (1 << 11), "syscall");
    ADD_FLAG(ext_edx & (1 << 20), "nx");
    ADD_FLAG(ext_edx & (1 << 26), "rdtscp");
    ADD_FLAG(ext_edx & (1 << 29), "lm");

#undef ADD_FLAG

    flags[fpos] = '\0';

    char tmp[2048];
    int sz = snprintf(tmp, sizeof(tmp),
                      "processor\t: 0\n"
                      "vendor_id\t: %s\n"
                      "cpu family\t: %u\n"
                      "model\t\t: %u\n"
                      "model name\t: %s\n"
                      "stepping\t: %u\n"
                      "cpu MHz\t\t: 1000.000\n"
                      "cache size\t: 0 KB\n"
                      "physical id\t: 0\n"
                      "siblings\t: 1\n"
                      "core id\t\t: 0\n"
                      "cpu cores\t: 1\n"
                      "apicid\t\t: %u\n"
                      "initial apicid\t: %u\n"
                      "flags\t\t: %s\n"
                      "\n",
                      vendor, family, model, brand, stepping, apicid, apicid, flags);
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_memstats_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    uint64_t pa = pmm_alloc_total();
    uint64_t pf = pmm_free_total();
    int64_t pd = (int64_t) (pa - pf);
    int64_t hd = heap_alloc_delta();
    char tmp[256];
    int sz = snprintf(tmp, sizeof(tmp),
                      "PhysAlloc: %lu\n"
                      "PhysFree:  %lu\n"
                      "PhysDelta: %ld\n"
                      "HeapDelta: %ld\n",
                      pa, pf, pd, hd);
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_meminfo_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    uint64_t total = pmm_usable_pages() * (PAGE_SIZE / 1024);
    uint64_t free = pmm_free_pages() * (PAGE_SIZE / 1024);
    uint64_t used = total > free ? total - free : 0;
    char tmp[1024];
    int sz = snprintf(tmp, sizeof(tmp),
                      "MemTotal:       %lu kB\n"
                      "MemFree:        %lu kB\n"
                      "MemAvailable:   %lu kB\n"
                      "Buffers:        0 kB\n"
                      "Cached:         0 kB\n"
                      "SwapCached:     0 kB\n"
                      "Active:         %lu kB\n"
                      "Inactive:       0 kB\n"
                      "Active(anon):   0 kB\n"
                      "Inactive(anon): 0 kB\n"
                      "Active(file):   0 kB\n"
                      "Inactive(file): 0 kB\n"
                      "Unevictable:    0 kB\n"
                      "Mlocked:        0 kB\n"
                      "SwapTotal:      0 kB\n"
                      "SwapFree:       0 kB\n"
                      "Dirty:          0 kB\n"
                      "Writeback:      0 kB\n"
                      "AnonPages:      0 kB\n"
                      "Mapped:         0 kB\n"
                      "Shmem:          0 kB\n"
                      "KReclaimable:   0 kB\n"
                      "Slab:           0 kB\n"
                      "SReclaimable:   0 kB\n"
                      "SUnreclaim:     0 kB\n",
                      total, free, free, used);
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_uptime_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    uint64_t ms = g_ticks;
    char tmp[64];
    int sz = snprintf(tmp, sizeof(tmp), "%lu.%02lu %lu.%02lu\n", ms / 1000, (ms / 10) % 100,
                      ms / 1000, (ms / 10) % 100);
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_loadavg_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    char tmp[96];
    int runnable = proc_count(PROC_READY) + proc_count(PROC_RUNNING);
    int alive = proc_alive_count();
    int sz =
        snprintf(tmp, sizeof(tmp), "0.00 0.00 0.00 %d/%d %d\n", runnable, alive, proc_last_pid());
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_stat_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
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

static int64_t proc_mounts_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    char tmp[2048];
    int sz = snprintf(tmp, sizeof(tmp),
                      "ramfs / ramfs rw 0 0\n"
                      "proc /proc proc rw,nosuid,nodev,noexec 0 0\n"
                      "sysfs /sys sysfs rw,nosuid,nodev,noexec 0 0\n"
                      "devtmpfs /dev devtmpfs rw,nosuid 0 0\n");
    mount_entry_t *mt = mount_table();
    int mc = mount_count();
    for (int i = 0; i < mc && sz < (int) sizeof(tmp) - 128; i++) {
        if (!mt[i].used) continue;
        sz += snprintf(tmp + sz, sizeof(tmp) - (uint64_t) sz, "%s %s %s rw 0 0\n", mt[i].source,
                       mt[i].target, mt[i].fstype);
    }
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_filesystems_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    static const char filesystems[] = "nodev\tproc\n"
                                      "nodev\tsysfs\n"
                                      "nodev\tdevtmpfs\n"
                                      "\tramfs\n";
    return read_buf(buf, len, off, filesystems, sizeof(filesystems) - 1);
}

static int64_t proc_devices_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
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

static int64_t proc_pids_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    char tmp[4096];
    int pos = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *p = &g_proctable[i];
        if (p->state == PROC_UNUSED) continue;
        if (!jail_can_see(g_current_proc, p)) continue;
        const char *name = p->exe_path[0] ? p->exe_path : "unknown";
        uint64_t pages = p->pages_alloc > p->pages_freed ? p->pages_alloc - p->pages_freed : 0;
        uint64_t mem_kb = pages * (PAGE_SIZE / 1024);
        int n = snprintf(tmp + pos, sizeof(tmp) - (uint64_t) pos, "%u %u %c %u %lu %s\n", p->pid,
                         p->ppid, proc_state_char(p), p->uid, mem_kb, name);
        if (n < 0 || (uint64_t) pos + (uint64_t) n >= sizeof(tmp)) break;
        pos += n;
    }
    return read_buf(buf, len, off, tmp, (uint64_t) pos);
}

static int64_t proc_kmsg_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    (void) buf;
    (void) len;
    (void) off;
    return 0;
}

static int64_t proc_ostype_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    static const char s[] = "Kyronix\n";
    return read_buf(buf, len, off, s, sizeof(s) - 1);
}

static int64_t proc_pid_max_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    static const char s[] = "32768\n";
    return read_buf(buf, len, off, s, sizeof(s) - 1);
}

static int64_t proc_osrelease_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    static const char s[] = KERNEL_VERSION "\n";
    return read_buf(buf, len, off, s, sizeof(s) - 1);
}

static int64_t proc_hostname_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    char hostname[66];
    uint64_t size = system_hostname_copy(hostname, sizeof(hostname) - 1);
    hostname[size++] = '\n';
    hostname[size] = '\0';
    return read_buf(buf, len, off, hostname, size);
}

static proc_t *node_to_proc(vfs_node_t *n) {
    if (n->fs_private) {
        uint32_t pid = (uint32_t)(uintptr_t) n->fs_private;
        return proc_find(pid);
    }
    return g_current_proc;
}

static int64_t proc_self_exe_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    proc_t *p = node_to_proc(n);
    if (!p || !p->exe_path[0]) { if (p && n->fs_private) proc_unref(p); return 0; }
    int64_t r = read_buf(buf, len, off, p->exe_path, strlen(p->exe_path));
    if (n->fs_private) proc_unref(p);
    return r;
}

static int64_t proc_self_cmdline_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    proc_t *p = node_to_proc(n);
    if (!p || !p->exe_path[0]) { if (p && n->fs_private) proc_unref(p); return 0; }
    int64_t r = read_buf(buf, len, off, p->exe_path, strlen(p->exe_path) + 1);
    if (n->fs_private) proc_unref(p);
    return r;
}

static int64_t proc_self_environ_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    (void) buf;
    (void) len;
    (void) off;
    return 0;
}

static int64_t proc_self_status_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    proc_t *p = node_to_proc(n);
    if (!p) return 0;
    char tmp[1024];
    int threads = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proctable[i].state != PROC_UNUSED && g_proctable[i].space == p->space) threads++;
    int sz = snprintf(
        tmp, sizeof(tmp),
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
        "Syscall:\t%lu\n"
        "VmPeak:\t0 kB\n"
        "VmSize:\t0 kB\n"
        "VmRSS:\t%lu kB\n"
        "VmLeak:\t%ld kB\n",
        proc_name(p), proc_state_char(p), p->pid, p->pid, p->ppid, p->uid, p->euid, p->suid,
        p->fsuid, p->gid, p->egid, p->sgid, p->fsgid, VFS_FD_MAX, threads ? threads : 1,
        p->pending_sigs, p->sig_mask, (unsigned long) p->ptrace_orig_rax,
        (unsigned long) ((p->pages_alloc * PAGE_SIZE) / 1024),
        (long) ((int64_t) (p->pages_alloc - p->pages_freed) * (int64_t) (PAGE_SIZE / 1024)));
    if (n->fs_private) proc_unref(p);
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_self_stat_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    proc_t *p = node_to_proc(n);
    if (!p) return 0;
    char tmp[512];
    int sz = snprintf(tmp, sizeof(tmp),
                      "%u (%s) %c %u %d 0 0 0 0 0 0 0 0 0 0 0 20 0 1 0 %lu 0 0 0 0 0 0 0 0 0 0 0 0 "
                      "0 0 0 0 0 0 0\n",
                      p->pid, proc_name(p), proc_state_char(p), p->ppid, p->pgid, g_ticks / 10);
    if (n->fs_private) proc_unref(p);
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_self_maps_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    proc_t *p = node_to_proc(n);
    if (!p) return 0;
    char tmp[768];
    const char *exe = p->exe_path[0] ? p->exe_path : "";
    // randomized addresses: don't leak actual VA to non-root
    int sz = snprintf(tmp, sizeof(tmp),
                      "00400000-00800000 r-xp 00000000 00:00 0 %s\n"
                      "xxxxxxxx-xxxxxxxx rw-p 00000000 00:00 0 [heap]\n"
                      "yyyyyyyy-yyyyyyyy rw-p 00000000 00:00 0 [stack]\n",
                      exe);
    if (n->fs_private) proc_unref(p);
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_self_pagemap_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    proc_t *p = node_to_proc(n);
    if (p && p->euid != 0) { if (n->fs_private) proc_unref(p); return -(int64_t) EPERM; }
    if (!p || !p->space || len < 8) { if (p && n->fs_private) proc_unref(p); return 0; }
    uint64_t nentries = len / 8;
    uint64_t written = 0;
    for (uint64_t i = 0; i < nentries; i++) {
        uint64_t va = ((off / 8) + i) << 12;
        uint64_t phys = vmm_virt_to_phys(p->space, va);
        uint64_t entry = phys ? ((phys >> 12) | (1ULL << 63)) : 0;
        memcpy(buf + i * 8, &entry, 8);
        written += 8;
    }
    if (n->fs_private) proc_unref(p);
    return (int64_t) written;
}

static uint16_t dirent_record_len(const char *name) {
    return (uint16_t) ((sizeof(struct linux_dirent64) + strlen(name) + 1 + 7) & ~7U);
}

static bool emit_dirent(uint8_t *out, uint64_t count, uint64_t *done, uint64_t ino, int64_t off,
                        uint8_t type, const char *name) {
    uint16_t rec = dirent_record_len(name);
    if (*done + rec > count) return false;
    struct linux_dirent64 *d = (struct linux_dirent64 *) (out + *done);
    uint64_t namelen = strlen(name) + 1;
    if (*done + sizeof(*d) + namelen > count) return false;
    d->d_ino = ino;
    d->d_off = off;
    d->d_reclen = rec;
    d->d_type = type;
    memcpy(d->d_name, name, namelen);
    *done += rec;
    return true;
}

bool procfs_getdents64(vfs_node_t *dir, uint64_t *pos, void *buf, uint64_t count, int *out) {
    char path[512];
    vfs_node_abspath(dir, path, sizeof(path));
    if (strcmp(path, "/proc/self/fd") != 0 && strcmp(path, "/dev/fd") != 0) return false;

    uint8_t *dst = (uint8_t *) buf;
    uint64_t done = 0;
    uint64_t emitted = 0;
    uint64_t idx = 0;
    uint64_t skip = *pos;

    if (idx++ >= skip && emit_dirent(dst, count, &done, dir->ino, (int64_t) idx, DT_DIR, "."))
        emitted++;
    if (idx++ >= skip && emit_dirent(dst, count, &done, dir->parent ? dir->parent->ino : dir->ino,
                                     (int64_t) idx, DT_DIR, ".."))
        emitted++;

    vfs_file_t **fds = vfs_get_fdtable();
    for (int fd = 0; fd < VFS_FD_MAX; fd++) {
        if (!fds || !fds[fd]) continue;
        char name[16];
        snprintf(name, sizeof(name), "%d", fd);
        if (idx++ < skip) continue;
        if (!emit_dirent(dst, count, &done, 100000u + (uint64_t) fd, (int64_t) idx, DT_LNK, name))
            break;
        emitted++;
    }

    *pos += emitted;
    *out = (int) done;
    return true;
}

static int parse_fd_link(const char *path) {
    const char *s = NULL;
    if (strncmp(path, "/proc/self/fd/", 14) == 0)
        s = path + 14;
    else if (strncmp(path, "/dev/fd/", 8) == 0)
        s = path + 8;
    else
        return -1;
    if (!*s) return -1;
    int fd = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        if (fd >= 102400) // prevent overflow - max fd is 1024
            return -1;
        fd = fd * 10 + (*s - '0');
        s++;
    }
    return fd;
}

bool procfs_readlink(const char *path, char *buf, uint64_t bufsz, int *out) {
    if (!uptr_ok_w(buf, bufsz)) {
        *out = -(int) EFAULT;
        return true;
    }

    if (strcmp(path, "/proc/self/exe") == 0) {
        proc_t *p = g_current_proc;
        if (!p || !p->exe_path[0]) {
            *out = -(int) ENOENT;
            return true;
        }
        uint64_t n = strlen(p->exe_path);
        if (n > bufsz) n = bufsz;
        memcpy(buf, p->exe_path, n);
        *out = (int) n;
        return true;
    }

    if (strncmp(path, "/proc/", 6) == 0 && strcmp(path + strlen(path) - 4, "/exe") == 0) {
        const char *start = path + 6;
        const char *end = path + strlen(path) - 4;
        uint32_t pid = 0;
        bool valid = start < end;
        for (const char *c = start; c < end && valid; c++) {
            if (*c < '0' || *c > '9') { valid = false; break; }
            pid = pid * 10 + (uint32_t) (*c - '0');
        }
        if (valid && pid > 0) {
            proc_t *p = proc_find(pid);
            if (!p) { *out = -(int) ENOENT; return true; }
            if (!p->exe_path[0]) { proc_unref(p); *out = -(int) ENOENT; return true; }
            uint64_t n = strlen(p->exe_path);
            if (n > bufsz) n = bufsz;
            memcpy(buf, p->exe_path, n);
            *out = (int) n;
            proc_unref(p);
            return true;
        }
    }

    int fd = parse_fd_link(path);
    if (fd < 0) return false;
    vfs_file_t *f = fd_get_file(fd);
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
    if (n > bufsz) n = bufsz;
    memcpy(buf, tmp, n);
    *out = (int) n;
    return true;
}

bool procfs_try_pid_dir(vfs_node_t *parent, const char *name) {
    if (!parent || !name || !*name) return false;

    // name must be purely numeric
    for (const char *c = name; *c; c++)
        if (*c < '0' || *c > '9') return false;

    uint32_t pid = 0;
    for (const char *c = name; *c; c++)
        pid = pid * 10 + (uint32_t) (*c - '0');

    proc_t *p = proc_find(pid);
    if (!p) return false;
    proc_unref(p);

    // create /proc/<N>/ directory
    char dir_path[128];
    snprintf(dir_path, sizeof(dir_path), "/proc/%s", name);
    vfs_node_t *dir = vfs_mkdir_p(dir_path, 0555);
    if (!dir) return false;

    // create stat, status, cmdline, exe inside the directory
    void *pid_tag = (void *) (uintptr_t) pid;

    char stat_path[160];
    snprintf(stat_path, sizeof(stat_path), "%s/stat", dir_path);
    vfs_node_t *stat = vfs_create_chr(stat_path, proc_self_stat_read, NULL);
    if (stat) stat->fs_private = pid_tag;

    char status_path[160];
    snprintf(status_path, sizeof(status_path), "%s/status", dir_path);
    vfs_node_t *status = vfs_create_chr(status_path, proc_self_status_read, NULL);
    if (status) status->fs_private = pid_tag;

    char cmdline_path[160];
    snprintf(cmdline_path, sizeof(cmdline_path), "%s/cmdline", dir_path);
    vfs_node_t *cmdline = vfs_create_chr(cmdline_path, proc_self_cmdline_read, NULL);
    if (cmdline) cmdline->fs_private = pid_tag;

    char exe_path[160];
    snprintf(exe_path, sizeof(exe_path), "%s/exe", dir_path);
    vfs_node_t *exe = vfs_create_chr(exe_path, proc_self_exe_read, NULL);
    if (exe) exe->fs_private = pid_tag;

    char maps_path[160];
    snprintf(maps_path, sizeof(maps_path), "%s/maps", dir_path);
    vfs_node_t *maps = vfs_create_chr(maps_path, proc_self_maps_read, NULL);
    if (maps) maps->fs_private = pid_tag;

    return true;
}

#ifdef CONFIG_KMEMLEAK
static int64_t proc_kmemleak_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    proc_t *p = g_current_proc;
    if (p && p->euid != 0) return -(int64_t) EPERM;

    static char *kmem_buf = NULL;
    static uint64_t kmem_sz = 0;

    if (off == 0) {
        uint64_t sz = 8192;
        for (;;) {
            char *nb = krealloc(kmem_buf, sz);
            if (!nb) {
                kfree(kmem_buf);
                kmem_buf = NULL;
                kmem_sz = 0;
                return -(int64_t) ENOMEM;
            }
            kmem_buf = nb;
            kmem_sz = sz;
            int leaked = kmemleak_report(kmem_buf, kmem_sz);
            (void) leaked;
            uint64_t used = strlen(kmem_buf);
            if (used + 128 < sz) break;
            if (sz >= 262144) break;
            sz *= 2;
        }
    }
    if (!kmem_buf) return 0;
    return read_buf(buf, len, off, kmem_buf, strlen(kmem_buf));
}
#endif

#ifdef CONFIG_PROFILER
static int64_t proc_profile_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    static char *prof_buf = NULL;
    static uint64_t prof_sz = 0;

    if (off == 0) {
        uint64_t sz = 16384;
        for (;;) {
            char *nb = krealloc(prof_buf, sz);
            if (!nb) {
                kfree(prof_buf);
                prof_buf = NULL;
                prof_sz = 0;
                return -(int64_t) ENOMEM;
            }
            prof_buf = nb;
            prof_sz = sz;
            int written = prof_render(prof_buf, prof_sz);
            if (written < (int) sz - 128) break;
            if (sz >= 262144) break;
            sz *= 2;
        }
    }
    if (!prof_buf) return 0;
    return read_buf(buf, len, off, prof_buf, strlen(prof_buf));
}

static int64_t proc_profile_write(vfs_node_t *n, const char *buf, uint64_t len, uint64_t pos) {
    (void) n;
    (void) pos;
    if (g_current_proc && g_current_proc->euid != 0) return -(int64_t) EPERM;
    if (!len || !uptr_ok(buf, 1)) return -(int64_t) EFAULT;

    if (len >= 5 && memcmp(buf, "start", 5) == 0) {
        prof_start();
    } else if (len >= 4 && memcmp(buf, "stop", 4) == 0) {
        prof_stop();
    } else if (len >= 5 && memcmp(buf, "reset", 5) == 0) {
        prof_reset();
    } else {
        return -(int64_t) EINVAL;
    }
    return (int64_t) len;
}
#endif

static const char *klog_level_name(int level) {
    switch (level) {
    case KLOG_ERROR:
        return "error";
    case KLOG_WARN:
        return "warn";
    case KLOG_INFO:
        return "info";
    case KLOG_DEBUG:
        return "debug";
    default:
        return "?";
    }
}

static int64_t proc_loglevel_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void) n;
    char tmp[32];
    int lvl = klog_get_level();
    int sz = snprintf(tmp, sizeof(tmp), "%d (%s)\n", lvl, klog_level_name(lvl));
    return read_buf(buf, len, off, tmp, (uint64_t) sz);
}

static int64_t proc_loglevel_write(vfs_node_t *n, const char *buf, uint64_t len, uint64_t pos) {
    (void) n;
    if (g_current_proc && g_current_proc->euid != 0) return -(int64_t) EPERM;
    if (!len || !uptr_ok(buf, 1)) return -(int64_t) EFAULT;
    char c = buf[0];
    int v = c - '0';
    if (v >= 0 && v <= 3) {
        klog_set_level(v);
        return (int64_t) len;
    }
    return -(int64_t) EINVAL;
}

void procfs_init(void) {
    vfs_mkdir_p("/proc", 0555);
    vfs_mkdir_p("/proc/self", 0555);
    vfs_mkdir_p("/proc/self/fd", 0500);
    vfs_mkdir_p("/proc/bus/pci", 0555);
    vfs_mkdir_p("/proc/bus/input", 0555);
    vfs_mkdir_p("/proc/sys/kernel", 0555);

    vfs_create_chr("/proc/version", proc_version_read, NULL);
    vfs_create_chr("/proc/bus/pci/devices", proc_pci_devices_read, NULL);
    {
        vfs_node_t *km = vfs_create_chr("/proc/kmsg", proc_kmsg_read, NULL);
        if (km) km->mode = S_IFCHR | 0400;
    }
    vfs_create_chr("/proc/cpuinfo", proc_cpuinfo_read, NULL);
    vfs_create_chr("/proc/pids", proc_pids_read, NULL);
    vfs_create_chr("/proc/meminfo", proc_meminfo_read, NULL);
    vfs_create_chr("/proc/memstats", proc_memstats_read, NULL);
    vfs_create_chr("/proc/uptime", proc_uptime_read, NULL);
    vfs_create_chr("/proc/loadavg", proc_loadavg_read, NULL);
    vfs_create_chr("/proc/stat", proc_stat_read, NULL);
    vfs_create_chr("/proc/mounts", proc_mounts_read, NULL);
    vfs_create_symlink("/proc/self/mounts", "/proc/mounts");
    vfs_create_chr("/proc/filesystems", proc_filesystems_read, NULL);
    vfs_create_chr("/proc/devices", proc_devices_read, NULL);
    vfs_create_chr("/proc/modules", module_proc_read, NULL);
    vfs_create_chr("/proc/sys/kernel/ostype", proc_ostype_read, NULL);
    vfs_create_chr("/proc/sys/kernel/osrelease", proc_osrelease_read, NULL);
    vfs_create_chr("/proc/sys/kernel/hostname", proc_hostname_read, NULL);
    vfs_create_chr("/proc/sys/kernel/pid_max", proc_pid_max_read, NULL);

    vfs_create_chr("/proc/self/exe", proc_self_exe_read, NULL);
    vfs_create_chr("/proc/self/cmdline", proc_self_cmdline_read, NULL);
    vfs_create_chr("/proc/self/environ", proc_self_environ_read, NULL);
    vfs_create_chr("/proc/self/status", proc_self_status_read, NULL);
    vfs_create_chr("/proc/self/stat", proc_self_stat_read, NULL);
    vfs_create_chr("/proc/self/maps", proc_self_maps_read, NULL);

    vfs_node_t *pm = vfs_create_chr("/proc/self/pagemap", proc_self_pagemap_read, NULL);
    if (pm) pm->mode = S_IFCHR | 0400;

    vfs_node_t *ll = vfs_create_chr("/proc/loglevel", proc_loglevel_read, proc_loglevel_write);
    if (ll) ll->mode = S_IFCHR | 0644;

#ifdef CONFIG_KMEMLEAK
    vfs_node_t *kml = vfs_create_chr("/proc/kmemleak", proc_kmemleak_read, NULL);
    if (kml) kml->mode = S_IFCHR | 0400;
#endif

#ifdef CONFIG_PROFILER
    vfs_node_t *pf = vfs_create_chr("/proc/profile", proc_profile_read, proc_profile_write);
    if (pf) pf->mode = S_IFCHR | 0644;
#endif
}
