#include "mem.h"

#include "fs/vfs.h"
#include "internal.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vma.h"
#include "mm/vmm.h"
#include "proc/proc.h"
#include "security/anti_toctou.h"

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define MAP_ANON 0x20
#define MAP_FIXED 0x10
#define MAP_PRIVATE 0x02
#define MAP_SHARED 0x01

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2

void syscall_set_brk(uint64_t brk_base) {
    proc_t *p = cur();
    if (p) p->brk = p->brk_base = PAGE_ALIGN_UP(brk_base);
}

int64_t sys_brk(uint64_t addr) {
    proc_t *p = cur();
    if (!p || !p->space) return -(int64_t) ENOMEM;
    if (addr == 0) return (int64_t) p->brk;
    if (addr < p->brk_base) return (int64_t) p->brk;
    if (addr >= USER_LIMIT || addr > USER_LIMIT - (PAGE_SIZE - 1))
        return (int64_t) p->brk;

    if (addr <= p->brk) {
        uint64_t new_end = PAGE_ALIGN_UP(addr);
        uint64_t old_end = PAGE_ALIGN_UP(p->brk);
        uint64_t nfreed = (old_end - new_end) / PAGE_SIZE;
        for (uint64_t va = new_end; va < old_end; va += PAGE_SIZE) {
            uint64_t phys = vmm_virt_to_phys(p->space, va);
            vmm_unmap(p->space, va);
            if (phys) pmm_free((void *) phys);
        }
        p->pages_freed += nfreed;
        if (old_end > p->brk_base)
            vma_remove_overlaps(p->space, p->brk_base, old_end - p->brk_base);
        if (new_end > p->brk_base)
            vma_add(p->space, p->brk_base, new_end - p->brk_base, PROT_READ | PROT_WRITE, 0, true);
        p->brk = addr;
        return (int64_t) p->brk;
    }

    uint64_t old = PAGE_ALIGN_UP(p->brk);
    uint64_t new = PAGE_ALIGN_UP(addr);
    uint64_t nallocd = 0;
    for (uint64_t va = old; va < new; va += PAGE_SIZE) {
        void *ph = pmm_alloc_zeroed();
        if (!ph) {
            for (uint64_t r = old; r < va; r += PAGE_SIZE) {
                uint64_t phys = vmm_virt_to_phys(p->space, r);
                vmm_unmap(p->space, r);
                if (phys) pmm_free((void *) phys);
            }
            return (int64_t) p->brk;
        }
        if (vmm_map(p->space, va, (uint64_t) ph, VMM_UDATA) < 0) {
            pmm_free(ph);
            for (uint64_t r = old; r < va; r += PAGE_SIZE) {
                uint64_t phys = vmm_virt_to_phys(p->space, r);
                vmm_unmap(p->space, r);
                if (phys) pmm_free((void *) phys);
            }
            return (int64_t) p->brk;
        }
        nallocd++;
    }
    p->pages_alloc += nallocd;
    if (old > p->brk_base) vma_remove_overlaps(p->space, p->brk_base, old - p->brk_base);
    vma_add(p->space, p->brk_base, new - p->brk_base, PROT_READ | PROT_WRITE, 0, true);
    p->brk = addr;
    return (int64_t) addr;
}

static bool user_map_range_ok(uint64_t addr, uint64_t len) {
    if (!len || (addr & (PAGE_SIZE - 1))) return false;
    if (addr >= USER_LIMIT || len > USER_LIMIT - addr) return false;
    return true;
}

static bool prot_valid(uint64_t prot) {
    if (prot & ~(uint64_t) (PROT_READ | PROT_WRITE | PROT_EXEC)) return false;
    return (prot & (PROT_WRITE | PROT_EXEC)) != (PROT_WRITE | PROT_EXEC);
}

static bool mmap_flags_valid(uint64_t flags) {
    if (flags & ~(uint64_t) (MAP_ANON | MAP_FIXED | MAP_PRIVATE | MAP_SHARED)) return false;
    uint64_t kind = flags & (MAP_PRIVATE | MAP_SHARED);
    return kind == MAP_PRIVATE || kind == MAP_SHARED;
}

static int mmap_pick_addr(proc_t *p, uint64_t addr, uint64_t length, uint64_t flags,
                          uint64_t *out) {
    if ((flags & MAP_FIXED) && addr) {
        uint64_t va = PAGE_ALIGN_DOWN(addr);
        if (!user_map_range_ok(va, length)) return -EINVAL;
        for (uint64_t o = 0; o < length; o += PAGE_SIZE) {
            uint64_t page = va + o;
            if (!vma_page_mapped(p->space, page) && vmm_virt_to_phys(p->space, page))
                return -EINVAL;
        }
        *out = va;
        return 0;
    }

    uint64_t va = PAGE_ALIGN_UP(p->mmap_bump);
    for (int tries = 0; tries < VMM_VMA_MAX + 16; tries++) {
        if (!user_map_range_ok(va, length)) return -ENOMEM;
        if (!vma_conflicts(p->space, va, length)) {
            p->mmap_bump = va + length;
            *out = va;
            return 0;
        }
        va += length;
    }
    return -ENOMEM;
}

static void unmap_owned_pages(proc_t *p, uint64_t addr, uint64_t len) {
    uint64_t nfreed = 0;
    for (uint64_t o = 0; o < len; o += PAGE_SIZE) {
        uint64_t va = addr + o;
        bool tracked = vma_page_mapped(p->space, va);
        bool owned = vma_page_owned(p->space, va);
        uint64_t phys = vmm_virt_to_phys(p->space, va);
        if (tracked) vmm_unmap(p->space, va);
        if (owned && phys) {
            pmm_free((void *) phys);
            nfreed++;
        }
    }
    p->pages_freed += nfreed;
}

static void rollback_new_mapping(proc_t *p, uint64_t addr, uint64_t mapped_len, uint64_t vma_len) {
    if (mapped_len) unmap_owned_pages(p, addr, mapped_len);
    vma_remove(p->space, addr, vma_len);
}

static int mmap_fixed_replace(proc_t *p, uint64_t addr, uint64_t len, uint64_t flags) {
    if (!(flags & MAP_FIXED)) return 0;
    unmap_owned_pages(p, addr, len);
    return vma_remove_overlaps(p->space, addr, len);
}

int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd,
                 uint64_t off) {
    (void) fd;
    (void) off;
    proc_t *p = cur();
    if (!p || !p->space) return -(int64_t) ENOMEM;
    if (!length) return -(int64_t) EINVAL;
    if (length > UINT64_MAX - (PAGE_SIZE - 1)) return -(int64_t) EINVAL;
    length = PAGE_ALIGN_UP(length);
    if (!prot_valid(prot) || !mmap_flags_valid(flags)) return -(int64_t) EINVAL;
    if ((off & (PAGE_SIZE - 1)) != 0) return -(int64_t) EINVAL;

    vfs_file_t *file = NULL;
    vfs_node_t *fn = NULL;
    if (!(flags & MAP_ANON)) {
        file = fd_get_file((int) fd);
        fn = file ? file->node : NULL;
        if (!fn) return -(int64_t) EBADF;
        if (fn->type != VFS_TYPE_REG && !(fn->type == VFS_TYPE_CHR && fn->chr_mmap))
            return -(int64_t) EBADF;
        if ((flags & MAP_SHARED) && (prot & PROT_WRITE) &&
            (file->flags & O_ACCMODE) == O_RDONLY)
            return -(int64_t) EACCES;
    }

    uint64_t va = 0;
    int pick = mmap_pick_addr(p, addr, length, flags, &va);
    if (pick < 0) return pick;
    int fixed_rc = mmap_fixed_replace(p, va, length, flags);
    if (fixed_rc < 0) return fixed_rc;

    uint64_t vf = VMM_UDATA;
    if (!(prot & PROT_WRITE)) vf &= ~(uint64_t) VMM_WRITE;
    if (prot & PROT_EXEC) vf &= ~(uint64_t) VMM_NX;
    bool reserve_only = (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0;

    if (!(flags & MAP_ANON)) {
        // chr-dev with custom mmap (e.g. uio physical bar mapping)
        if (fn->type == VFS_TYPE_CHR && fn->chr_mmap) {
            int rc = vma_add(p->space, va, length, (uint32_t) prot, (uint32_t) flags, false);
            if (rc < 0) return rc;
            if (reserve_only) return (int64_t) va;
            int64_t mr = fn->chr_mmap(fn, off, length, va, vf);
            if (mr < 0) {
                vma_remove(p->space, va, length);
                return mr;
            }
            return mr;
        }

        if (fn->type != VFS_TYPE_REG) return -(int64_t) EBADF;

        int rc = vma_add(p->space, va, length, (uint32_t) prot, (uint32_t) flags, true);
        if (rc < 0) return rc;
        if (reserve_only) return (int64_t) va;

        // lazily load file data if filesystem hasnt populated fn->data yet (ext2)
        if (!fn->data && fn->fs_ops && fn->fs_ops->read) fn->fs_ops->read(fn, NULL, 0, 0);

        // file-backed: MAP_PRIVATE - allocate pages and copy file content
        uint64_t file_size = fn->size;
        uint64_t nallocd_file = 0;
        for (uint64_t o = 0; o < length; o += PAGE_SIZE) {
            void *ph = pmm_alloc_zeroed();
            if (!ph) {
                rollback_new_mapping(p, va, o, length);
                return -(int64_t) ENOMEM;
            }
            if (vmm_map(p->space, va + o, (uint64_t) ph, vf) < 0) {
                pmm_free(ph);
                rollback_new_mapping(p, va, o, length);
                return -(int64_t) ENOMEM;
            }
            if (fn->data && off < file_size && o < file_size - off) {
                uint64_t file_off = off + o;
                uint64_t copy = file_size - file_off;
                if (copy > PAGE_SIZE) copy = PAGE_SIZE;
                memcpy(phys_to_virt((uint64_t) ph), fn->data + file_off, copy);
            }
            nallocd_file++;
        }
        p->pages_alloc += nallocd_file;
        return (int64_t) va;
    }

    int rc = vma_add(p->space, va, length, (uint32_t) prot, (uint32_t) flags, true);
    if (rc < 0) return rc;
    if (reserve_only) return (int64_t) va;

    uint64_t nallocd = 0;
    for (uint64_t o = 0; o < length; o += PAGE_SIZE) {
        void *ph = pmm_alloc_zeroed();
        if (!ph) {
            rollback_new_mapping(p, va, o, length);
            return -(int64_t) ENOMEM;
        }
        if (vmm_map(p->space, va + o, (uint64_t) ph, vf) < 0) {
            pmm_free(ph);
            rollback_new_mapping(p, va, o, length);
            return -(int64_t) ENOMEM;
        }
        nallocd++;
    }
    p->pages_alloc += nallocd;
    return (int64_t) va;
}

int64_t sys_munmap(uint64_t addr, uint64_t len) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EINVAL;
    if ((addr & (PAGE_SIZE - 1)) || !len) return -(int64_t) EINVAL;
    if (len > UINT64_MAX - (PAGE_SIZE - 1)) return -(int64_t) EINVAL;
    len = PAGE_ALIGN_UP(len);
    if (!user_map_range_ok(addr, len)) return -(int64_t) EINVAL;
    unmap_owned_pages(p, addr, len);
    vma_remove_overlaps(p->space, addr, len);
    return 0;
}

int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    proc_t *p = cur();
    if (!p || !len) return 0;
    if (addr & (PAGE_SIZE - 1)) return -(int64_t) EINVAL;
    if (!prot_valid(prot)) return -(int64_t) EINVAL;
    addr = PAGE_ALIGN_DOWN(addr);
    if (len > UINT64_MAX - (PAGE_SIZE - 1)) return -(int64_t) EINVAL;
    len = PAGE_ALIGN_UP(len);
    if (!user_map_range_ok(addr, len)) return -(int64_t) EINVAL;
    anti_toctou_observe_memory(addr, ANTI_TOCTOU_MEM_PROTECT);
    bool tracked = vma_range_ok(p->space, addr, len);
    if (!tracked && !vmm_user_range_ok(p->space, addr, len, false)) return -(int64_t) EINVAL;
    if (tracked) {
        int rc = vma_protect(p->space, addr, len, (uint32_t) prot);
        if (rc < 0) return rc;
    }
    uint64_t flags = VMM_USER | VMM_NX;
    if (prot & PROT_WRITE) flags |= VMM_WRITE;
    if (prot & PROT_EXEC) flags &= ~(uint64_t) VMM_NX;
    for (uint64_t o = 0; o < len; o += PAGE_SIZE) {
        uint64_t va = addr + o;
        if (!vmm_virt_to_phys(p->space, va)) {
            if (tracked) continue;
            return -(int64_t) EINVAL;
        }
        if (vmm_protect(p->space, va, flags) < 0) return -(int64_t) EINVAL;
    }
    return 0;
}

int64_t sys_madvise(void *addr, uint64_t len, int advice) {
    (void) addr;
    (void) len;
    (void) advice;
    return 0;
}

int64_t sys_mremap(uint64_t old_addr, uint64_t old_sz, uint64_t new_sz, uint64_t flags,
                   uint64_t new_addr) {
    proc_t *p = cur();
    if (!p || !old_sz || !new_sz) return -(int64_t) EINVAL;
    if (flags & ~(uint64_t) (MREMAP_MAYMOVE | MREMAP_FIXED)) return -(int64_t) EINVAL;
    if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE)) return -(int64_t) EINVAL;
    if ((old_addr & (PAGE_SIZE - 1)) || ((flags & MREMAP_FIXED) && (new_addr & (PAGE_SIZE - 1))))
        return -(int64_t) EINVAL;
    if (old_sz > UINT64_MAX - (PAGE_SIZE - 1) ||
        new_sz > UINT64_MAX - (PAGE_SIZE - 1))
        return -(int64_t) EINVAL;
    old_sz = PAGE_ALIGN_UP(old_sz);
    new_sz = PAGE_ALIGN_UP(new_sz);
    uint32_t old_prot, old_map_flags;
    bool old_owned;
    if (!user_map_range_ok(old_addr, old_sz) ||
        !vma_range_info(p->space, old_addr, old_sz, &old_prot, &old_map_flags, &old_owned) ||
        !old_owned)
        return -(int64_t) EINVAL;
    if (new_sz <= old_sz) {
        uint64_t tail = old_sz - new_sz;
        if (tail) {
            unmap_owned_pages(p, old_addr + new_sz, tail);
            int rc = vma_remove(p->space, old_addr + new_sz, tail);
            if (rc < 0) return rc;
        }
        return (int64_t) old_addr;
    }
    if (!(flags & MREMAP_MAYMOVE)) return -(int64_t) ENOMEM;
    uint64_t new_va;
    if (flags & MREMAP_FIXED) {
        new_va = new_addr;
        if (!user_map_range_ok(new_va, new_sz) || vma_conflicts(p->space, new_va, new_sz))
            return -(int64_t) EINVAL;
    } else {
        int pick = mmap_pick_addr(p, 0, new_sz, 0, &new_va);
        if (pick < 0) return pick;
    }

    int rc = vma_add(p->space, new_va, new_sz, old_prot, old_map_flags, true);
    if (rc < 0) return rc;

    uint64_t page_flags = VMM_USER | VMM_NX;
    if (old_prot & PROT_WRITE) page_flags |= VMM_WRITE;
    if (old_prot & PROT_EXEC) page_flags &= ~(uint64_t) VMM_NX;
    for (uint64_t o = 0; o < new_sz; o += PAGE_SIZE) {
        void *ph = pmm_alloc_zeroed();
        if (!ph) {
            rollback_new_mapping(p, new_va, o, new_sz);
            return -(int64_t) ENOMEM;
        }
        if (vmm_map(p->space, new_va + o, (uint64_t) ph, page_flags) < 0) {
            pmm_free(ph);
            rollback_new_mapping(p, new_va, o, new_sz);
            return -(int64_t) ENOMEM;
        }
        if (o < old_sz) {
            uint64_t src = vmm_virt_to_phys(p->space, old_addr + o);
            if (src) memcpy(phys_to_virt((uint64_t) ph), phys_to_virt(src), PAGE_SIZE);
        }
    }
    unmap_owned_pages(p, old_addr, old_sz);
    rc = vma_remove(p->space, old_addr, old_sz);
    if (rc < 0) return rc;
    p->pages_alloc += new_sz / PAGE_SIZE;
    return (int64_t) new_va;
}
