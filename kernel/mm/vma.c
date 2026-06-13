#include "vma.h"
#include "lib/string.h"

#define ENOMEM 12
#define EINVAL 22

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

static bool range_end(uint64_t start, uint64_t len, uint64_t* end)
{
    if (!len)
        return false;
    *end = start + len;
    return *end > start;
}

static vmm_vma_t* empty_slot(vmm_space_t* sp)
{
    for (int i = 0; i < VMM_VMA_MAX; i++)
        if (!sp->vmas[i].used)
            return &sp->vmas[i];
    return NULL;
}

static vmm_vma_t* containing(vmm_space_t* sp, uint64_t addr)
{
    for (int i = 0; i < VMM_VMA_MAX; i++) {
        vmm_vma_t* v = &sp->vmas[i];
        if (v->used && addr >= v->start && addr < v->end)
            return v;
    }
    return NULL;
}

void vma_reset(vmm_space_t* sp)
{
    if (sp)
        memset(sp->vmas, 0, sizeof(sp->vmas));
}

void vma_copy(vmm_space_t* dst, const vmm_space_t* src)
{
    if (dst && src)
        memcpy(dst->vmas, src->vmas, sizeof(dst->vmas));
}

bool vma_conflicts(vmm_space_t* sp, uint64_t start, uint64_t len)
{
    uint64_t end;
    if (!sp || !range_end(start, len, &end))
        return true;

    for (int i = 0; i < VMM_VMA_MAX; i++) {
        vmm_vma_t* v = &sp->vmas[i];
        if (!v->used)
            continue;
        if (start < v->end && end > v->start)
            return true;
    }
    return false;
}

int vma_add(vmm_space_t* sp, uint64_t start, uint64_t len, uint32_t prot, uint32_t map_flags,
            bool free_on_unmap)
{
    uint64_t end;
    if (!sp || !range_end(start, len, &end))
        return -EINVAL;
    if (vma_conflicts(sp, start, len))
        return -EINVAL;

    vmm_vma_t* v = empty_slot(sp);
    if (!v)
        return -ENOMEM;

    v->start = start;
    v->end = end;
    v->prot = prot;
    v->map_flags = map_flags;
    v->used = 1;
    v->free_on_unmap = free_on_unmap ? 1 : 0;
    return 0;
}

bool vma_range_ok(vmm_space_t* sp, uint64_t start, uint64_t len)
{
    uint64_t end;
    if (!sp || !range_end(start, len, &end))
        return false;

    uint64_t cur = start;
    while (cur < end) {
        vmm_vma_t* v = containing(sp, cur);
        if (!v)
            return false;
        cur = v->end < end ? v->end : end;
    }
    return true;
}

bool vma_page_mapped(vmm_space_t* sp, uint64_t addr)
{
    return containing(sp, addr) != NULL;
}

bool vma_page_owned(vmm_space_t* sp, uint64_t addr)
{
    vmm_vma_t* v = containing(sp, addr);
    return v && v->free_on_unmap;
}

bool vma_page_fault_allowed(vmm_space_t* sp, uint64_t addr, bool write, bool exec)
{
    vmm_vma_t* v = containing(sp, addr);
    if (!v || !v->free_on_unmap)
        return false;
    if (write && !(v->prot & PROT_WRITE))
        return false;
    if (exec && !(v->prot & PROT_EXEC))
        return false;
    return (v->prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) != 0;
}

uint64_t vma_page_flags(vmm_space_t* sp, uint64_t addr)
{
    vmm_vma_t* v = containing(sp, addr);
    uint64_t flags = VMM_USER | VMM_NX;
    if (!v)
        return flags;
    if (v->prot & PROT_WRITE)
        flags |= VMM_WRITE;
    if (v->prot & PROT_EXEC)
        flags &= ~(uint64_t) VMM_NX;
    return flags;
}

static int split_for_hole(vmm_space_t* sp, vmm_vma_t* v, uint64_t start, uint64_t end)
{
    bool left = start > v->start;
    bool right = end < v->end;

    if (left && right && !empty_slot(sp))
        return -ENOMEM;

    uint64_t old_end = v->end;
    if (!left && !right) {
        memset(v, 0, sizeof(*v));
        return 0;
    }
    if (left && !right) {
        v->end = start;
        return 0;
    }
    if (!left && right) {
        v->start = end;
        return 0;
    }

    vmm_vma_t* r = empty_slot(sp);
    *r = *v;
    r->start = end;
    r->end = old_end;
    v->end = start;
    return 0;
}

int vma_remove(vmm_space_t* sp, uint64_t start, uint64_t len)
{
    uint64_t end;
    if (!sp || !range_end(start, len, &end))
        return -EINVAL;
    if (!vma_range_ok(sp, start, len))
        return -EINVAL;

    uint64_t cur = start;
    while (cur < end) {
        vmm_vma_t* v = containing(sp, cur);
        if (!v)
            return -EINVAL;
        uint64_t cut_end = v->end < end ? v->end : end;
        int rc = split_for_hole(sp, v, cur, cut_end);
        if (rc < 0)
            return rc;
        cur = cut_end;
    }
    return 0;
}

int vma_remove_overlaps(vmm_space_t* sp, uint64_t start, uint64_t len)
{
    uint64_t end;
    if (!sp || !range_end(start, len, &end))
        return -EINVAL;

    for (int i = 0; i < VMM_VMA_MAX; i++) {
        vmm_vma_t* v = &sp->vmas[i];
        if (!v->used)
            continue;
        if (start >= v->end || end <= v->start)
            continue;

        uint64_t cut_start = start > v->start ? start : v->start;
        uint64_t cut_end = end < v->end ? end : v->end;
        int rc = split_for_hole(sp, v, cut_start, cut_end);
        if (rc < 0)
            return rc;
    }
    return 0;
}

int vma_protect(vmm_space_t* sp, uint64_t start, uint64_t len, uint32_t prot)
{
    uint64_t end;
    if (!sp || !range_end(start, len, &end))
        return -EINVAL;
    if (!vma_range_ok(sp, start, len))
        return -EINVAL;

    uint64_t cur = start;
    while (cur < end) {
        vmm_vma_t* v = containing(sp, cur);
        if (!v)
            return -EINVAL;
        uint64_t cut_end = v->end < end ? v->end : end;
        uint32_t map_flags = v->map_flags;
        bool free_on_unmap = v->free_on_unmap != 0;
        int rc = split_for_hole(sp, v, cur, cut_end);
        if (rc < 0)
            return rc;
        rc = vma_add(sp, cur, cut_end - cur, prot, map_flags, free_on_unmap);
        if (rc < 0)
            return rc;
        cur = cut_end;
    }
    return 0;
}
