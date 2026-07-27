#include "procctl.h"

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/syscall_setup.h"
#include "exec/elf.h"
#include "exec/process.h"
#include "fs/vfs.h"
#include "fs/vfs_internal.h"
#include "futex.h"
#include "internal.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/shm.h"
#include "mm/vma.h"
#include "mm/vmm.h"
#include "proc/jail.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "proc/smp.h"
#include "security/phantom.h"

static void proc_release_fdtable(proc_t *p) {
    if (!p || !p->fds) return;
    if (p->fds_refcnt && *p->fds_refcnt > 1) {
        (*p->fds_refcnt)--;
    } else {
        vfs_free_fdtable(p->fds);
        if (p->fds_refcnt) kfree(p->fds_refcnt);
    }
    p->fds = NULL;
    p->fds_refcnt = NULL;
}

static void proc_discard_embryo(proc_t *p) {
    if (!p) return;
    proc_release_fdtable(p);
    if (p->space && !p->is_thread) vmm_space_free(p->space);
    proc_kstack_free(p);
    spin_lock(&g_proctable_lock);
    memset(p, 0, sizeof(*p));
    p->state = PROC_UNUSED;
    proc_clear_used(p);
    spin_unlock(&g_proctable_lock);
}

static int64_t sys_fork_at_mode(proc_t *parent, syscall_frame_t *f,
                                uint64_t child_stack, bool cow,
                                uint64_t child_rax, bool publish,
                                proc_t **prepared_child) {
    if (!parent) return -(int64_t) ENOMEM;
    if (!jail_can_fork(parent->jail_id)) return -(int64_t) EAGAIN;

    proc_t *child = proc_alloc(parent->pid);
    if (!child) return -(int64_t) ENOMEM;

    child->space = vmm_space_new();
    if (!child->space) goto fail_space;

    if ((cow ? vmm_fork_user_cow(child->space, parent->space)
             : vmm_fork_user(child->space, parent->space)) < 0)
        goto fail_fork;

    vfs_copy_fdtable(child->fds, parent->fds);
    if (child->fds_refcnt) *child->fds_refcnt = 1;

    child->brk = parent->brk;
    child->brk_base = parent->brk_base;
    child->mmap_bump = parent->mmap_bump;
    child->pgid = parent->pgid;
    child->user_rsp = child_stack ? child_stack : cpu_get_user_rsp();

    child->sig_mask = parent->sig_mask;
    memcpy(child->sig_actions, parent->sig_actions, sizeof(parent->sig_actions));
    child->pending_sigs = 0;
    child->fs_base = parent->fs_base;
    child->uid = parent->uid;
    child->euid = parent->euid;
    child->suid = parent->suid;
    child->gid = parent->gid;
    child->egid = parent->egid;
    child->sgid = parent->sgid;
    child->fsuid = parent->fsuid;
    child->fsgid = parent->fsgid;
    child->umask = parent->umask;
    memcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));
    child->jail_id = parent->jail_id;
    child->jail_exempt = parent->jail_exempt;
    jail_ref(child->jail_id);

    uint8_t *ksp = child->kstack + KSTACK_SIZE;

    ksp -= sizeof(syscall_frame_t);
    syscall_frame_t *cf = (syscall_frame_t *) ksp;
    *cf = *f;
    cf->rax = child_rax;

    ksp -= 8;
    *(uint64_t *) ksp = (uint64_t) (uintptr_t) proc_resume_frame;

    ksp -= 6 * 8;
    memset(ksp, 0, 6 * 8);

    child->kstack_rsp = (uint64_t) ksp;
    if (prepared_child) *prepared_child = child;
    if (publish) proc_publish_ready(child);

    return (int64_t) child->pid;

fail_fork:
fail_space:
    proc_discard_embryo(child);
    return -(int64_t) ENOMEM;
}

static int64_t sys_fork_at(syscall_frame_t *f, uint64_t child_stack) {
    return sys_fork_at_mode(cur(), f, child_stack, false, 0, true, NULL);
}

int64_t sys_fork(syscall_frame_t *f) { return sys_fork_at(f, 0); }

static int64_t proc_phantom_commit(proc_t *parent, proc_t *child, int64_t pid,
                                   uint64_t fault_address, uint64_t fault_error) {
    char root[128];
    snprintf(root, sizeof(root), "/tmp/phantom/%u", (uint32_t) pid);
    char path[192];
    int jid = -1;

    if (!vfs_mkdir_p("/tmp/phantom", 0755) || !vfs_mkdir_p(root, 0755))
        goto fail_closed;
    snprintf(path, sizeof(path), "%s/etc", root);
    if (!vfs_mkdir_p(path, 0755)) goto fail_closed;
    snprintf(path, sizeof(path), "%s/etc/ssl", root);
    if (!vfs_mkdir_p(path, 0755)) goto fail_closed;
    snprintf(path, sizeof(path), "%s/etc/hostname", root);
    if (!vfs_create_file(path, 0644, "phantom-sandbox\n", 16)) goto fail_closed;
    snprintf(path, sizeof(path), "%s/etc/ssl/phantom.key", root);
    if (!vfs_create_file(path, 0600, "PHANTOM-DUMMY-KEY\n", 18)) goto fail_closed;

    kjail_conf_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = JAILF_ALL;
    cfg.max_procs = 1;
    strncpy(cfg.root, root, sizeof(cfg.root) - 1);
    strncpy(cfg.name, "phantom", sizeof(cfg.name) - 1);
    jid = jail_create(parent->jail_id, &cfg, parent->euid);
    if (jid <= 0) goto fail_closed;
    jail_enter(child, (uint32_t) jid);
    child->jail_exempt = 0;
    child->phantom_sandbox = 1;
    int sanitized = vfs_phantom_sanitize_fdtable(child->fds);
    if (sanitized < 0) goto fail_closed;
    if (fault_error &&
        vmm_phantom_relax_page(child->space, fault_address,
                               (fault_error & 2u) != 0,
                               (fault_error & 16u) != 0) < 0)
        goto fail_closed;
    uint32_t clone_flags = PHANTOM_CLONEF_COW | PHANTOM_CLONEF_COMMITTED;
    if (fault_error)
        clone_flags |= PHANTOM_CLONEF_FAULT | PHANTOM_CLONEF_WORKER;
    phantom_record_for(parent, PHANTOM_EVENT_CLONE, clone_flags,
                       (uint64_t) (uint32_t) pid,
                       (uint64_t) (uint32_t) jid,
                       fault_error ? "fault sandbox committed"
                                   : "COW sandbox committed");
    if (sanitized > 0)
        phantom_record_for(parent, PHANTOM_EVENT_CLONE,
                           PHANTOM_CLONEF_FDS_SANITIZED, 0,
                           (uint64_t) (uint32_t) sanitized,
                           "inherited descriptors sanitized");
    proc_publish_ready(child);
    return pid;

fail_closed:
    phantom_record_for(parent, PHANTOM_EVENT_CLONE,
                       PHANTOM_CLONEF_FAILED |
                           (fault_error ? PHANTOM_CLONEF_WORKER : 0),
                       (uint64_t) (uint32_t) pid, 0,
                       "sandbox setup failed closed");
    if (jid > 0) {
        jail_unref(child->jail_id);
        jail_remove((uint32_t) jid, parent);
    } else {
        jail_unref(child->jail_id);
    }
    snprintf(path, sizeof(path), "%s/etc/ssl/phantom.key", root);
    vfs_unlink(path);
    snprintf(path, sizeof(path), "%s/etc/hostname", root);
    vfs_unlink(path);
    snprintf(path, sizeof(path), "%s/etc/ssl", root);
    vfs_rmdir(path);
    snprintf(path, sizeof(path), "%s/etc", root);
    vfs_rmdir(path);
    vfs_rmdir(root);
    proc_discard_embryo(child);
    return -(int64_t) ENOMEM;
}

int64_t proc_phantom_clone(syscall_frame_t *f) {
    proc_t *parent = cur();
    if (!parent) return -(int64_t) EPERM;
    proc_t *child = NULL;
    int64_t pid =
        sys_fork_at_mode(parent, f, 0, true, 0, false, &child);
    if (pid < 0) return pid;
    return proc_phantom_commit(parent, child, pid, 0, 0);
}

int64_t proc_phantom_fault_clone(proc_t *source, const cpu_state_t *state,
                                 uint64_t address) {
    if (!source || !state) return -(int64_t) EPERM;

    syscall_frame_t placeholder;
    memset(&placeholder, 0, sizeof(placeholder));
    proc_t *child = NULL;
    int64_t pid = sys_fork_at_mode(source, &placeholder, state->rsp, true,
                                   state->rax, false, &child);
    if (pid < 0) return pid;

    uint8_t *ksp = child->kstack + KSTACK_SIZE;
    ksp -= sizeof(cpu_state_t);
    *(cpu_state_t *) ksp = *state;
    ksp -= 8;
    *(uint64_t *) ksp = (uint64_t) (uintptr_t) proc_resume_interrupt_frame;
    ksp -= 6 * 8;
    memset(ksp, 0, 6 * 8);
    child->kstack_rsp = (uint64_t) ksp;
    child->user_rsp = state->rsp;

    return proc_phantom_commit(source, child, pid, address,
                               state->error_code);
}

int64_t sys_phantom_clone(syscall_frame_t *f) {
    if (!host_priv()) return -(int64_t) EPERM;
    return proc_phantom_clone(f);
}

#define CLONE_VM 0x00000100
#define CLONE_FILES 0x00000400
#define CLONE_THREAD 0x00010000
#define CLONE_SETTLS 0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID 0x01000000

int64_t sys_clone(uint64_t flags, uint64_t child_stack, uint32_t *ptid, uint32_t *ctid,
                  uint64_t newtls, syscall_frame_t *f) {
    proc_t *parent = cur();
    if (!parent) return -(int64_t) ENOMEM;

    if (!(flags & CLONE_THREAD)) return sys_fork_at(f, child_stack);
    if ((flags & CLONE_PARENT_SETTID) && (!ptid || !uptr_ok_w(ptid, sizeof(*ptid))))
        return -(int64_t) EFAULT;
    if ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) && ctid &&
        !uptr_ok_w(ctid, sizeof(*ctid)))
        return -(int64_t) EFAULT;
    if (!jail_can_fork(parent->jail_id)) return -(int64_t) EAGAIN;

    proc_t *child = proc_alloc(parent->pid);
    if (!child) return -(int64_t) ENOMEM;

    child->space = parent->space; /* shared address space */
    child->is_thread = 1;

    if (flags & CLONE_FILES) {
        kfree(child->fds);
        kfree(child->fds_refcnt);
        child->fds = parent->fds;
        child->fds_refcnt = parent->fds_refcnt;
        if (child->fds_refcnt) (*child->fds_refcnt)++;
    } else {
        vfs_copy_fdtable(child->fds, parent->fds);
        if (child->fds_refcnt) *child->fds_refcnt = 1;
    }

    child->brk = parent->brk;
    child->brk_base = parent->brk_base;
    child->mmap_bump = parent->mmap_bump;
    child->pgid = parent->pgid;
    child->sig_mask = parent->sig_mask;
    memcpy(child->sig_actions, parent->sig_actions, sizeof(parent->sig_actions));
    child->pending_sigs = 0;
    child->fs_base = (flags & CLONE_SETTLS) ? newtls : parent->fs_base;
    child->uid = parent->uid;
    child->euid = parent->euid;
    child->suid = parent->suid;
    child->gid = parent->gid;
    child->egid = parent->egid;
    child->sgid = parent->sgid;
    child->fsuid = parent->fsuid;
    child->fsgid = parent->fsgid;
    child->umask = parent->umask;
    memcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));
    child->jail_id = parent->jail_id;
    child->jail_exempt = parent->jail_exempt;
    jail_ref(child->jail_id);

    if ((flags & CLONE_PARENT_SETTID) && ptid) *ptid = child->pid;
    if ((flags & CLONE_CHILD_SETTID) && ctid) *ctid = child->pid;
    if (flags & CLONE_CHILD_CLEARTID) child->cleartid_addr = ctid;

    uint8_t *ksp = child->kstack + KSTACK_SIZE;
    ksp -= sizeof(syscall_frame_t);
    syscall_frame_t *cf = (syscall_frame_t *) ksp;
    *cf = *f;
    cf->rax = 0;

    child->user_rsp = child_stack ? child_stack : parent->user_rsp;

    ksp -= 8;
    *(uint64_t *) ksp = (uint64_t) (uintptr_t) proc_resume_frame;
    ksp -= 6 * 8;
    memset(ksp, 0, 6 * 8);
    child->kstack_rsp = (uint64_t) ksp;
    proc_publish_ready(child);

    log_info("[clone] parent=%u child=%u flags=0x%lx", parent->pid, child->pid, flags);
    return (int64_t) child->pid;
}

#define MAX_EXEC_ARGS 32
#define MAX_EXEC_STRLEN 4096

static bool exec_vec_slot(const char **uvec, int i, const char **out) {
    const char **slot = uvec + i;
    if (!uptr_ok(slot, sizeof(*slot))) return false;
    *out = *slot;
    return true;
}

static size_t exec_str_len(const char *s) {
    if (!s) return 0;
    for (size_t i = 0; i < MAX_EXEC_STRLEN; i++) {
        const char *a = s + i;
        if (i == 0 || ((uint64_t) (uintptr_t) a & 0xFFF) == 0) {
            if (!uptr_ok(a, 1)) return 0;
        }
        if (!*a) return i + 1;
    }
    return 0;
}

int64_t sys_execve(const char *path, const char **uargv, const char **uenvp) {
    if (!path) return -(int64_t) EFAULT;

    char abs[512];
    if (!path_abs(abs, path)) return -(int64_t) EFAULT;
    const char *exec_path = abs;

    vfs_node_t *node = vfs_lookup(exec_path);
    if (!node || node->type != VFS_TYPE_REG) {
        vfs_node_unref_internal(node);
        return -(int64_t) ENOENT;
    }
    /* lazy-load file data if needed */
    if (!node->data && node->fs_ops && node->fs_ops->read)
        node->fs_ops->read(node, NULL, 0, node->size);
    if (!node->data) {
        vfs_node_unref_internal(node);
        return -(int64_t) ENOENT;
    }
    int exec_perm = vfs_access(exec_path, 1);
    if (exec_perm < 0) {
        vfs_node_unref_internal(node);
        return (int64_t) exec_perm;
    }

    char shebang_interp[512] = { 0 };
    char shebang_arg[256] = { 0 };
    char script_path[512] = { 0 };
    bool is_shebang = false;

    if (node->size >= 2 && node->data[0] == '#' && node->data[1] == '!') {
        char line[256];
        size_t ll = 0;
        while (ll < 254 && 2 + ll < node->size) {
            char ch = (char) node->data[2 + ll];
            if (ch == '\n' || ch == '\r') break;
            line[ll++] = ch;
        }
        line[ll] = '\0';
        char *p2 = line;
        while (*p2 == ' ' || *p2 == '\t') p2++;
        if (!*p2) {
            vfs_node_unref_internal(node);
            return -(int64_t) ENOEXEC;
        }
        char *istart = p2;
        while (*p2 && *p2 != ' ' && *p2 != '\t') p2++;
        size_t ilen = (size_t) (p2 - istart);
        if (!ilen || ilen >= sizeof(shebang_interp)) {
            vfs_node_unref_internal(node);
            return -(int64_t) ENOEXEC;
        }
        memcpy(shebang_interp, istart, ilen);
        while (*p2 == ' ' || *p2 == '\t') p2++;
        if (*p2) {
            strncpy(shebang_arg, p2, sizeof(shebang_arg) - 1);
            char *e = shebang_arg + strlen(shebang_arg) - 1;
            while (e >= shebang_arg && (*e == ' ' || *e == '\t' || *e == '\r')) *e-- = '\0';
        }
        strncpy(script_path, exec_path, sizeof(script_path) - 1);
        vfs_node_unref_internal(node);
        node = vfs_lookup(shebang_interp);
        if (!node || node->type != VFS_TYPE_REG) {
            vfs_node_unref_internal(node);
            return -(int64_t) ENOENT;
        }
        /* lazy-load interpreter if needed */
        if (!node->data && node->fs_ops && node->fs_ops->read)
            node->fs_ops->read(node, NULL, 0, node->size);
        if (!node->data) {
            log_error("execve: interp '%s' has no data", shebang_interp);
            vfs_node_unref_internal(node);
            return -(int64_t) ENOENT;
        }
        exec_path = shebang_interp;
        is_shebang = true;
    }

    char *argv_mem[MAX_EXEC_ARGS] = { 0 };
    char *envp_mem[MAX_EXEC_ARGS] = { 0 };
    const char *kargv[MAX_EXEC_ARGS + 1];
    const char *kenvp[MAX_EXEC_ARGS + 1];
    int argc = 0, envc = 0;

#define FREE_EXEC_STRS()                                                                           \
    do {                                                                                           \
        for (int _i = 0; _i < MAX_EXEC_ARGS; _i++) {                                               \
            if (argv_mem[_i]) {                                                                    \
                kfree(argv_mem[_i]);                                                               \
                argv_mem[_i] = NULL;                                                               \
            }                                                                                      \
            if (envp_mem[_i]) {                                                                    \
                kfree(envp_mem[_i]);                                                               \
                envp_mem[_i] = NULL;                                                               \
            }                                                                                      \
        }                                                                                          \
    } while (0)

#define COPY_EXEC_STR(dst_arr, dst_vec, idx, uptr)                                                  \
    do {                                                                                           \
        size_t _n = exec_str_len(uptr);                                                            \
        if (!_n) {                                                                                 \
            vfs_node_unref_internal(node);                                                         \
            FREE_EXEC_STRS();                                                                      \
            return -(int64_t) EFAULT;                                                              \
        }                                                                                          \
        (dst_arr)[idx] = kmalloc(_n);                                                              \
        if (!(dst_arr)[idx]) {                                                                     \
            vfs_node_unref_internal(node);                                                         \
            FREE_EXEC_STRS();                                                                      \
            return -(int64_t) ENOMEM;                                                              \
        }                                                                                          \
        memcpy((dst_arr)[idx], (uptr), _n);                                                        \
        (dst_arr)[idx][_n - 1] = '\0';                                                             \
        (dst_vec)[idx] = (dst_arr)[idx];                                                           \
    } while (0)

#define NEXT_EXEC_SLOT(uvec, i, out)                                                               \
    do {                                                                                           \
        if (!exec_vec_slot((uvec), (i), &(out))) {                                                 \
            vfs_node_unref_internal(node);                                                         \
            FREE_EXEC_STRS();                                                                      \
            return -(int64_t) EFAULT;                                                              \
        }                                                                                          \
    } while (0)

    if (is_shebang) {
        kargv[argc++] = shebang_interp;
        if (shebang_arg[0]) kargv[argc++] = shebang_arg;
        kargv[argc++] = script_path;
        int ui = 1;
        for (;;) {
            if (!uargv || argc >= MAX_EXEC_ARGS) break;
            const char *us = NULL;
            NEXT_EXEC_SLOT(uargv, ui, us);
            if (!us) break;
            COPY_EXEC_STR(argv_mem, kargv, argc, us);
            argc++;
            ui++;
        }
    } else {
        if (uargv) {
            for (;;) {
                if (argc >= MAX_EXEC_ARGS) break;
                const char *us = NULL;
                NEXT_EXEC_SLOT(uargv, argc, us);
                if (!us) break;
                COPY_EXEC_STR(argv_mem, kargv, argc, us);
                argc++;
            }
        }
        if (argc == 0) {
            size_t n = strlen(exec_path) + 1;
            argv_mem[0] = kmalloc(n);
            if (!argv_mem[0]) {
                vfs_node_unref_internal(node);
                FREE_EXEC_STRS();
                return -(int64_t) ENOMEM;
            }
            memcpy(argv_mem[0], exec_path, n);
            kargv[0] = argv_mem[0];
            argc = 1;
        }
    }
    kargv[argc] = NULL;

    if (uenvp) {
        for (;;) {
            if (envc >= MAX_EXEC_ARGS) break;
            const char *us = NULL;
            NEXT_EXEC_SLOT(uenvp, envc, us);
            if (!us) break;
            COPY_EXEC_STR(envp_mem, kenvp, envc, us);
            envc++;
        }
    }
    kenvp[envc] = NULL;
#undef COPY_EXEC_STR
#undef NEXT_EXEC_SLOT

    elf_load_result_t res;
    if (elf_load(node->data, node->size, &res) < 0) {
        vfs_node_unref_internal(node);
        FREE_EXEC_STRS();
        return -(int64_t) ENOEXEC;
    }
    vfs_node_unref_internal(node); /* data is now mapped; release our ref */
    node = NULL;

    if (res.interp[0]) {
        vfs_node_t *inode = vfs_lookup(res.interp);
        if (!inode || inode->type != VFS_TYPE_REG) {
            vfs_node_unref_internal(inode);
            vmm_space_free(res.space);
            FREE_EXEC_STRS();
            return -(int64_t) ENOENT;
        }
        /* lazy-load interpreter data if needed */
        if (!inode->data && inode->fs_ops && inode->fs_ops->read)
            inode->fs_ops->read(inode, NULL, 0, inode->size);
        if (!inode->data) {
            vfs_node_unref_internal(inode);
            vmm_space_free(res.space);
            FREE_EXEC_STRS();
            return -(int64_t) ENOENT;
        }
        elf_load_result_t ires;
        memset(&ires, 0, sizeof(ires));
        if (elf_load_into(res.space, inode->data, inode->size, 0x7f0000000000ULL, &ires) < 0) {
            vfs_node_unref_internal(inode);
            vmm_space_free(res.space);
            FREE_EXEC_STRS();
            return -(int64_t) ENOEXEC;
        }
        vfs_node_unref_internal(inode);
        res.entry = ires.prog_entry;
        res.interp_base = 0x7f0000000000ULL;
    }

    uint64_t rsp = setup_user_stack(res.space, &res, argc, (const char *const *) kargv,
                                    (const char *const *) kenvp);
    FREE_EXEC_STRS();

    if (!rsp) {
        vmm_space_free(res.space);
        return -(int64_t) ENOMEM;
    }

    proc_t *p = cur();
    vmm_space_t *old = p->space;

    p->space = res.space;
    p->brk = PAGE_ALIGN_UP(res.brk);
    p->brk_base = p->brk;
    p->mmap_bump = 0x0000500000000000ULL + ((kern_rand64() & 0x1FFULL) << 21);
    strncpy(p->exe_path, exec_path, sizeof(p->exe_path) - 1);
    g_current_space = p->space;

    vmm_switch(p->space);
    vmm_space_free(old);

    vfs_cloexec_flush();

    for (int i = 0; i < NSIG; i++) {
        if (p->sig_actions[i].sa_handler != SIG_IGN) {
            p->sig_actions[i].sa_handler = SIG_DFL;
            p->sig_actions[i].sa_flags = 0;
            p->sig_actions[i].sa_restorer = 0;
            p->sig_actions[i].sa_mask = 0;
        }
    }

    fpu_init(); /* exec gives the new image a clean, all-masked FPU/SSE state */

    if (g_jail_auto_isolate && !p->jail_exempt) {
        kjail_conf_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.flags = JAILF_ALL; /* empty root => same fs view, fresh pid/ipc namespace */
        int jid = jail_create(p->jail_id, &cfg, p->euid);
        if (jid > 0) jail_enter(p, (uint32_t) jid);
    }

    log_info("[exec] pid=%u entry=0x%lx rsp=0x%lx jail=%u", p->pid, res.entry, rsp, p->jail_id);

    if (p->tracer_pid) {
        syscall_frame_t exec_frame = { 0 };
        exec_frame.rcx = res.entry;
        exec_frame.r11 = 0x202ULL;
        p->user_rsp = rsp;
        proc_ptrace_stop(p, SIGTRAP, 1, &exec_frame, &exec_frame.r11);
        enter_userspace_exec(exec_frame.rcx, p->user_rsp, exec_frame.r11);
    }

    enter_userspace_exec(res.entry, rsp, 0x202ULL);
}

__attribute__((noreturn)) void proc_do_exit(int code) {
    proc_t *p = cur();
    log_info("[pid %u] exit(%d)", p->pid, code);

    /* clear stale pipe waiting pointer before slot is reused */
    if (p->blocked_pipe) {
        pipe_t *bp = (pipe_t *) p->blocked_pipe;
        if (p->blocked_pipe_read) {
            if (bp->waiting_reader == p) bp->waiting_reader = NULL;
        } else {
            if (bp->waiting_writer == p) bp->waiting_writer = NULL;
        }
        p->blocked_pipe = NULL;
    }

    shm_proc_exit(p->pid);
    if (p->phantom_sandbox) {
        jail_t *sandbox = jail_find(p->jail_id);
        if (sandbox) {
            char root[JAIL_ROOT_MAX];
            strncpy(root, sandbox->root, sizeof(root) - 1);
            root[sizeof(root) - 1] = '\0';
            char path[320];
            snprintf(path, sizeof(path), "%s/etc/ssl/phantom.key", root);
            vfs_unlink(path);
            snprintf(path, sizeof(path), "%s/etc/hostname", root);
            vfs_unlink(path);
            snprintf(path, sizeof(path), "%s/etc/ssl", root);
            vfs_rmdir(path);
            snprintf(path, sizeof(path), "%s/etc", root);
            vfs_rmdir(path);
            vfs_rmdir(root);
            jail_retire(p->jail_id);
        }
    }
    jail_unref(p->jail_id);
    proc_release_fdtable(p);

    spin_lock(&g_proctable_lock);
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *child = &g_proctable[i];
        if (child->state != PROC_UNUSED && child->ppid == p->pid) child->ppid = 1;
    }
    spin_unlock(&g_proctable_lock);

    if (p->is_thread) {
        if (p->cleartid_addr) {
            *p->cleartid_addr = 0;
            cleartid_wake(p->cleartid_addr);
        }
        p->state = PROC_DYING;
        proc_clear_ready(p);
        proc_defer_thread_reap(p);
        proc_t *nt = sched_claim_next(p);
        if (nt) {
            vfs_set_fdtable(nt->fds);
            g_current_space = nt->space;
            cpu_set_kernel_stack(nt->kstack_top);
            sched_switch(nt);
        }
        /* unreachable: this thread is dead and never scheduled again */
        cpu_halt();
    }

    p->exit_code = code;
    p->state = PROC_ZOMBIE;
    proc_clear_ready(p);

    proc_t *parent = proc_find(p->ppid);
    if (parent && parent->state != PROC_UNUSED) {
        proc_send_signal(parent, SIGCHLD);
        proc_unref(parent);
    }

    // switch to this cpu's idle process
    uint32_t cpu_id = this_cpu_id();
    proc_t *idle = (proc_t *) g_cpu_local[cpu_id].idle;
    if (idle && idle->state == PROC_READY) {
        idle->state = PROC_RUNNING;
        vfs_set_fdtable(idle->fds);
        g_current_space = idle->space;
        cpu_set_kernel_stack(idle->kstack_top);
        sched_switch(idle);
    }

    // fallback
    for (;;) {
        proc_t *next = sched_claim_next(p);
        if (next) {
            vfs_set_fdtable(next->fds);
            g_current_space = next->space;
            cpu_set_kernel_stack(next->kstack_top);
            sched_switch(next);
        }
        sti();
        hlt();
        cli();
    }
    cpu_halt();
}

int64_t sys_wait4(int pid, int *wstatus, int options, void *rusage) {
    if (wstatus && !uptr_ok_w(wstatus, sizeof(*wstatus))) return -(int64_t) EFAULT;
    if (rusage && !uptr_ok_w(rusage, 144)) return -(int64_t) EFAULT;
    proc_t *parent = cur();
    if (!parent) return -(int64_t) ECHILD;

    while (1) {
        bool any_child = false;
        for (int i = 0; i < PROC_MAX; i++) {
            proc_t *c = &g_proctable[i];
            if (c->state == PROC_UNUSED || c->state == PROC_EMBRYO) continue;
            bool is_tracee = c->tracer_pid == parent->pid;
            if (c->ppid != parent->pid && !is_tracee) continue;
            if (!jail_can_see(parent, c)) continue;
            if (pid > 0 && (int) c->pid != pid) continue;
            any_child = true;

            if (c->state == PROC_ZOMBIE) {
                if (wstatus) *wstatus = (c->exit_code & 0xFF) << 8;
                uint32_t cpid = c->pid;
                uint32_t child_jid = c->jail_id;
                spin_lock(&g_proctable_lock);
                proc_release_fdtable(c);
                vmm_space_free(c->space);
                proc_kstack_free(c);
                memset(c, 0, sizeof(*c));
                c->state = PROC_UNUSED;
                proc_clear_used(c);
                spin_unlock(&g_proctable_lock);
                jail_reap(child_jid);
                return (int64_t) cpid;
            }

            if (is_tracee && c->ptrace_stopped && !c->ptrace_reported) {
                c->ptrace_reported = 1;
                if (wstatus) *wstatus = ((c->ptrace_stop_sig & 0xFF) << 8) | 0x7f;
                return (int64_t) c->pid;
            }

            if ((options & 2) /* WUNTRACED */ && c->job_stopped && !c->stop_reported) {
                c->stop_reported = 1;
                if (wstatus) *wstatus = ((c->stop_sig & 0xFF) << 8) | 0x7f;
                return (int64_t) c->pid;
            }
        }

        if (!any_child) return -(int64_t) ECHILD;

        if (options & 1) /* WNOHANG */
            return 0;

        sched_yield_blocking();
    }
}

#define ARCH_SET_FS 0x1002
#define ARCH_SET_GS 0x1001
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

int64_t sys_arch_prctl(int code, uint64_t addr) {
    switch (code) {
    case ARCH_SET_FS:
        if (addr >= USER_LIMIT) return -(int64_t) EPERM;
        wrmsr(0xC0000100, addr);
        cur()->fs_base = addr;
        return 0;
    case ARCH_SET_GS:
        if (addr >= USER_LIMIT) return -(int64_t) EPERM;
        wrmsr(0xC0000101, addr);
        return 0;
    case ARCH_GET_FS:
        if (!uptr_ok_w((void *) addr, sizeof(uint64_t))) return -(int64_t) EFAULT;
        *(uint64_t *) addr = cur()->fs_base;
        return 0;
    case ARCH_GET_GS:
        if (!uptr_ok_w((void *) addr, sizeof(uint64_t))) return -(int64_t) EFAULT;
        *(uint64_t *) addr = rdmsr(0xC0000101);
        return 0;
    }
    return -(int64_t) EINVAL;
}
