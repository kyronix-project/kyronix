#include "vma.h"
#include "lib/string.h"
#include "mm/heap.h"

#define ENOMEM 12
#define EINVAL 22

static int slot_index(vmm_space_t *sp, const vmm_vma_t *v) {
    if (v < sp->vmas || v >= sp->vmas + VMM_VMA_MAX) return -1;
    return (int) (v - sp->vmas);
}

static void meta_free(vmm_space_t *sp, int slot) {
    if (sp->vma_meta[slot]) {
        kfree(sp->vma_meta[slot]);
        sp->vma_meta[slot] = NULL;
    }
}

static const char *ident_name(const vma_ident_t *id) { return (id && id->name) ? id->name : ""; }

static bool ident_buildable(const vma_ident_t *id) {
    if (!id || id->kind == VMA_KIND_NONE) return false;
    if ((id->kind == VMA_KIND_ANON || id->kind == VMA_KIND_FILE) && !ident_name(id)[0]) return false;
    return true;
}

static vma_meta_t *meta_build(const vma_ident_t *id) {
    if (!ident_buildable(id)) return NULL;
    const char *name = ident_name(id);
    size_t nlen = strlen(name);
    vma_meta_t *m = kmalloc(sizeof(*m) + nlen + 1);
    if (!m) return NULL;
    m->file_off = id->file_off;
    m->dev_major = id->dev_major;
    m->dev_minor = id->dev_minor;
    m->inode = id->inode;
    m->kind = id->kind;
    if (nlen) memcpy(m->name, name, nlen);
    m->name[nlen] = '\0';
    return m;
}

static void ident_from_slot(vmm_space_t *sp, int slot, vma_ident_t *out) {
    memset(out, 0, sizeof(*out));
    const vma_meta_t *m = sp->vma_meta[slot];
    if (!m) {
        out->kind = VMA_KIND_ANON;
        return;
    }
    out->kind = m->kind;
    out->file_off = m->file_off;
    out->dev_major = m->dev_major;
    out->dev_minor = m->dev_minor;
    out->inode = m->inode;
    out->name = m->name;
}

static bool ident_equal(const vma_ident_t *a, const vma_ident_t *b) {
    if (!!ident_buildable(a) != !!ident_buildable(b)) return false;
    if (!ident_buildable(a)) return true;
    return a->kind == b->kind && a->dev_major == b->dev_major && a->dev_minor == b->dev_minor &&
           a->inode == b->inode && strcmp(ident_name(a), ident_name(b)) == 0;
}

const vma_meta_t *vma_get_meta(vmm_space_t *sp, uint64_t addr) {
    if (!sp) return NULL;
    for (int i = 0; i < VMM_VMA_MAX; i++) {
        vmm_vma_t *v = &sp->vmas[i];
        if (v->used && addr >= v->start && addr < v->end) return sp->vma_meta[i];
    }
    return NULL;
}

static bool range_end(uint64_t start, uint64_t len, uint64_t *end) {
    if (!len) return false;
    *end = start + len;
    return *end > start;
}

static vmm_vma_t *empty_slot(vmm_space_t *sp) {
    for (int i = 0; i < VMM_VMA_MAX; i++)
        if (!sp->vmas[i].used) return &sp->vmas[i];
    return NULL;
}

static void meta_move(vmm_space_t *sp, int dst, int src, uint64_t delta) {
    meta_free(sp, dst);
    const vma_meta_t *s = (src >= 0) ? sp->vma_meta[src] : NULL;
    if (!s) return;
    size_t nlen = strlen(s->name);
    vma_meta_t *m = kmalloc(sizeof(*m) + nlen + 1);
    if (!m) return;
    memcpy(m, s, sizeof(*m));
    memcpy(m->name, s->name, nlen + 1);
    if (m->kind == VMA_KIND_FILE) m->file_off += delta;
    sp->vma_meta[dst] = m;
}

static vmm_vma_t *containing(vmm_space_t *sp, uint64_t addr) {
    for (int i = 0; i < VMM_VMA_MAX; i++) {
        vmm_vma_t *v = &sp->vmas[i];
        if (v->used && addr >= v->start && addr < v->end) return v;
    }
    return NULL;
}

void vma_reset(vmm_space_t *sp) {
    if (!sp) return;
    for (int i = 0; i < VMM_VMA_MAX; i++) meta_free(sp, i);
    memset(sp->vmas, 0, sizeof(sp->vmas));
}

void vma_copy(vmm_space_t *dst, const vmm_space_t *src) {
    if (!dst || !src) return;
    for (int i = 0; i < VMM_VMA_MAX; i++) meta_free(dst, i);
    memcpy(dst->vmas, src->vmas, sizeof(dst->vmas));
    for (int i = 0; i < VMM_VMA_MAX; i++) {
        dst->vma_meta[i] = NULL;
        const vma_meta_t *s = src->vma_meta[i];
        if (!s) continue;
        size_t nlen = strlen(s->name);
        vma_meta_t *m = kmalloc(sizeof(*m) + nlen + 1);
        if (!m) continue;
        memcpy(m, s, sizeof(*m));
        memcpy(m->name, s->name, nlen + 1);
        dst->vma_meta[i] = m;
    }
}

bool vma_conflicts(vmm_space_t *sp, uint64_t start, uint64_t len) {
    uint64_t end;
    if (!sp || !range_end(start, len, &end)) return true;

    for (int i = 0; i < VMM_VMA_MAX; i++) {
        vmm_vma_t *v = &sp->vmas[i];
        if (!v->used) continue;
        if (start < v->end && end > v->start) return true;
    }
    return false;
}

static bool vma_same_kind(const vmm_vma_t *v, uint32_t prot, uint32_t map_flags,
                          bool free_on_unmap) {
    return v->prot == prot && v->map_flags == map_flags &&
           v->free_on_unmap == (free_on_unmap ? 1 : 0);
}

int vma_add(vmm_space_t *sp, uint64_t start, uint64_t len, uint32_t prot, uint32_t map_flags,
            bool free_on_unmap, const vma_ident_t *id) {
    uint64_t end;
    if (!sp || !range_end(start, len, &end)) return -EINVAL;
    if (vma_conflicts(sp, start, len)) return -EINVAL;

    /*
     * Coalesce with the neighbours when everything but the range matches.
     * mmap() hands out consecutive addresses, so without this a program doing
     * many small mappings (musl's mallocng does exactly that) exhausts the
     * fixed slot table and every further mmap fails with ENOMEM.
     *
     * Neighbours must also agree on identity, and file mappings must be
     * contiguous in the file: merging two ranges backed by different files, or
     * two halves of one file with a gap between them, would make
     * /proc/<pid>/maps report a mapping that does not exist. That costs a few
     * more slots in programs that mmap many files, and is the honest trade --
     * the alternative is a maps file that lies.
     */
    vmm_vma_t *before = NULL, *after = NULL;
    for (int i = 0; i < VMM_VMA_MAX; i++) {
        vmm_vma_t *v = &sp->vmas[i];
        if (!v->used || !vma_same_kind(v, prot, map_flags, free_on_unmap)) continue;
        vma_ident_t ex;
        ident_from_slot(sp, i, &ex);
        if (!ident_equal(&ex, id)) continue;
        uint64_t want = id ? id->file_off : 0;
        if (v->end == start) {
            if (ex.kind == VMA_KIND_FILE && ex.file_off + (v->end - v->start) != want) continue;
            before = v;
        } else if (v->start == end) {
            if (ex.kind == VMA_KIND_FILE && want + len != ex.file_off) continue;
            after = v;
        }
    }
    if (before && after) {
        int ai = slot_index(sp, after);
        before->end = after->end;
        meta_free(sp, ai);
        memset(after, 0, sizeof(*after));
        return 0;
    }
    if (before) {
        before->end = end;
        return 0;
    }
    if (after) {
        int ai = slot_index(sp, after);
        if (sp->vma_meta[ai] && sp->vma_meta[ai]->kind == VMA_KIND_FILE) {
            sp->vma_meta[ai]->file_off = id ? id->file_off : 0;
        }
        after->start = start;
        return 0;
    }

    vmm_vma_t *v = empty_slot(sp);
    if (!v) return -ENOMEM;

    v->start = start;
    v->end = end;
    v->prot = prot;
    v->map_flags = map_flags;
    v->used = 1;
    v->free_on_unmap = free_on_unmap ? 1 : 0;
    int vi = slot_index(sp, v);
    meta_free(sp, vi);
    sp->vma_meta[vi] = meta_build(id);
    return 0;
}

bool vma_range_ok(vmm_space_t *sp, uint64_t start, uint64_t len) {
    uint64_t end;
    if (!sp || !range_end(start, len, &end)) return false;

    uint64_t cur = start;
    while (cur < end) {
        vmm_vma_t *v = containing(sp, cur);
        if (!v) return false;
        cur = v->end < end ? v->end : end;
    }
    return true;
}

bool vma_page_mapped(vmm_space_t *sp, uint64_t addr) { return containing(sp, addr) != NULL; }

int vma_used_count(vmm_space_t *sp) {
    int n = 0;
    if (!sp) return 0;
    for (int i = 0; i < VMM_VMA_MAX; i++)
        if (sp->vmas[i].used) n++;
    return n;
}

bool vma_lookup(vmm_space_t *sp, uint64_t addr, uint64_t *start, uint64_t *end, uint32_t *prot) {
    vmm_vma_t *v = containing(sp, addr);
    if (!v) return false;
    if (start) *start = v->start;
    if (end) *end = v->end;
    if (prot) *prot = v->prot;
    return true;
}

bool vma_page_owned(vmm_space_t *sp, uint64_t addr) {
    vmm_vma_t *v = containing(sp, addr);
    return v && v->free_on_unmap;
}

bool vma_range_info(vmm_space_t *sp, uint64_t start, uint64_t len, uint32_t *prot,
                    uint32_t *map_flags, bool *free_on_unmap) {
    uint64_t end;
    if (!sp || !range_end(start, len, &end)) return false;
    vmm_vma_t *first = containing(sp, start);
    if (!first) return false;
    uint32_t p = first->prot;
    uint32_t mf = first->map_flags;
    bool owned = first->free_on_unmap != 0;
    uint64_t cur = start;
    while (cur < end) {
        vmm_vma_t *v = containing(sp, cur);
        if (!v || v->prot != p || v->map_flags != mf || (v->free_on_unmap != 0) != owned)
            return false;
        cur = v->end < end ? v->end : end;
    }
    if (prot) *prot = p;
    if (map_flags) *map_flags = mf;
    if (free_on_unmap) *free_on_unmap = owned;
    return true;
}

bool vma_page_fault_allowed(vmm_space_t *sp, uint64_t addr, bool write, bool exec) {
    vmm_vma_t *v = containing(sp, addr);
    if (!v || !v->free_on_unmap) return false;
    if (write && !(v->prot & PROT_WRITE)) return false;
    if (exec && !(v->prot & PROT_EXEC)) return false;
    return (v->prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) != 0;
}

uint64_t vma_page_flags(vmm_space_t *sp, uint64_t addr) {
    vmm_vma_t *v = containing(sp, addr);
    uint64_t flags = VMM_USER | VMM_NX;
    if (!v) return flags;
    if (v->prot & PROT_WRITE) flags |= VMM_WRITE;
    if (v->prot & PROT_EXEC) flags &= ~(uint64_t) VMM_NX;
    return flags;
}

static int split_for_hole(vmm_space_t *sp, vmm_vma_t *v, uint64_t start, uint64_t end) {
    bool left = start > v->start;
    bool right = end < v->end;

    if (left && right && !empty_slot(sp)) return -ENOMEM;

    uint64_t old_start = v->start;
    uint64_t old_end = v->end;
    int vi = slot_index(sp, v);

    if (!left && !right) {
        meta_free(sp, vi);
        memset(v, 0, sizeof(*v));
        return 0;
    }
    if (left && !right) {
        v->end = start;
        return 0;
    }
    if (!left && right) {
        v->start = end;
        if (sp->vma_meta[vi] && sp->vma_meta[vi]->kind == VMA_KIND_FILE)
            sp->vma_meta[vi]->file_off += end - old_start;
        return 0;
    }

    vmm_vma_t *r = empty_slot(sp);
    *r = *v;
    r->start = end;
    r->end = old_end;
    v->end = start;
    meta_move(sp, slot_index(sp, r), vi, end - old_start);
    return 0;
}

int vma_remove(vmm_space_t *sp, uint64_t start, uint64_t len) {
    uint64_t end;
    if (!sp || !range_end(start, len, &end)) return -EINVAL;
    if (!vma_range_ok(sp, start, len)) return -EINVAL;

    uint64_t cur = start;
    while (cur < end) {
        vmm_vma_t *v = containing(sp, cur);
        if (!v) return -EINVAL;
        uint64_t cut_end = v->end < end ? v->end : end;
        int rc = split_for_hole(sp, v, cur, cut_end);
        if (rc < 0) return rc;
        cur = cut_end;
    }
    return 0;
}

int vma_remove_overlaps(vmm_space_t *sp, uint64_t start, uint64_t len) {
    uint64_t end;
    if (!sp || !range_end(start, len, &end)) return -EINVAL;

    for (int i = 0; i < VMM_VMA_MAX; i++) {
        vmm_vma_t *v = &sp->vmas[i];
        if (!v->used) continue;
        if (start >= v->end || end <= v->start) continue;

        uint64_t cut_start = start > v->start ? start : v->start;
        uint64_t cut_end = end < v->end ? end : v->end;
        int rc = split_for_hole(sp, v, cut_start, cut_end);
        if (rc < 0) return rc;
    }
    return 0;
}

int vma_protect(vmm_space_t *sp, uint64_t start, uint64_t len, uint32_t prot) {
    uint64_t end;
    if (!sp || !range_end(start, len, &end)) return -EINVAL;
    if (!vma_range_ok(sp, start, len)) return -EINVAL;

    uint64_t cur = start;
    while (cur < end) {
        vmm_vma_t *v = containing(sp, cur);
        if (!v) return -EINVAL;
        uint64_t cut_end = v->end < end ? v->end : end;
        uint32_t map_flags = v->map_flags;
        bool free_on_unmap = v->free_on_unmap != 0;
        vma_ident_t id;
        char name_buf[256];
        ident_from_slot(sp, slot_index(sp, v), &id);
        if (id.name) {
            strncpy(name_buf, id.name, sizeof(name_buf) - 1);
            name_buf[sizeof(name_buf) - 1] = '\0';
            id.name = name_buf;
        }
        if (id.kind == VMA_KIND_FILE) id.file_off += cur - v->start;
        int rc = split_for_hole(sp, v, cur, cut_end);
        if (rc < 0) return rc;
        rc = vma_add(sp, cur, cut_end - cur, prot, map_flags, free_on_unmap, &id);
        if (rc < 0) return rc;
        cur = cut_end;
    }
    return 0;
}
