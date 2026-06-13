#include "shm.h"
#include "../lib/string.h"
#include "../proc/proc.h"
#include "pmm.h"
#include "vmm.h"

#define EINVAL 22
#define ENOENT 2
#define EEXIST 17
#define ENOMEM 12
#define EPERM 1

#define IPC_PRIVATE 0
#define IPC_CREAT 01000
#define IPC_EXCL 02000
#define IPC_RMID 0
#define IPC_STAT 2
#define SHM_RDONLY 010000
#define SHM_RND 020000

#define SHM_MAX_SEGS 64
#define SHM_MAX_PAGES 4096 /* up to 16 mb per segment */
#define SHM_MAX_ATTACH (SHM_MAX_SEGS * 4)

typedef struct {
    int shmid;
    int key;
    uint64_t size;
    int n_pages;
    void* pages[SHM_MAX_PAGES];
    int ref_count;
    int destroy_pending;
    uint32_t mode;
} shm_seg_t;

typedef struct {
    int shmid;
    uint64_t va;
    uint32_t pid;
    int n_pages;
} shm_attach_t;

static shm_seg_t g_segs[SHM_MAX_SEGS];
static shm_attach_t g_attaches[SHM_MAX_ATTACH];
static int g_next_shmid = 1;

static shm_seg_t* seg_by_id(int id)
{
    for (int i = 0; i < SHM_MAX_SEGS; i++)
        if (g_segs[i].shmid == id)
            return &g_segs[i];
    return NULL;
}

static shm_seg_t* seg_by_key(int key)
{
    for (int i = 0; i < SHM_MAX_SEGS; i++)
        if (g_segs[i].shmid && g_segs[i].key == key)
            return &g_segs[i];
    return NULL;
}

static void seg_destroy(shm_seg_t* s)
{
    for (int i = 0; i < s->n_pages; i++)
        if (s->pages[i])
            pmm_free(s->pages[i]);
    memset(s, 0, sizeof(*s));
}

int sys_shmget(int key, uint64_t size, int flags)
{
    if (size == 0)
        size = PAGE_SIZE;
    uint64_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages > SHM_MAX_PAGES)
        return -EINVAL;

    if (key != IPC_PRIVATE) {
        shm_seg_t* s = seg_by_key(key);
        if (s) {
            if ((flags & IPC_CREAT) && (flags & IPC_EXCL))
                return -EEXIST;
            return s->shmid;
        }
        if (!(flags & IPC_CREAT))
            return -ENOENT;
    }

    shm_seg_t* slot = NULL;
    for (int i = 0; i < SHM_MAX_SEGS; i++)
        if (!g_segs[i].shmid) {
            slot = &g_segs[i];
            break;
        }
    if (!slot)
        return -ENOMEM;

    slot->n_pages = (int) npages;
    for (int i = 0; i < (int) npages; i++) {
        slot->pages[i] = pmm_alloc_zeroed();
        if (!slot->pages[i]) {
            for (int j = 0; j < i; j++)
                pmm_free(slot->pages[j]);
            return -ENOMEM;
        }
    }
    slot->shmid = g_next_shmid++;
    slot->key = key;
    slot->size = size;
    slot->mode = (uint32_t) (flags & 0777);
    return slot->shmid;
}

uint64_t sys_shmat(int shmid, uint64_t shmaddr, int shmflg)
{
    shm_seg_t* s = seg_by_id(shmid);
    if (!s)
        return (uint64_t) (-(int64_t) EINVAL);

    proc_t* p = g_current_proc;
    if (!p || !p->space)
        return (uint64_t) (-(int64_t) EINVAL);

    shm_attach_t* slot = NULL;
    for (int i = 0; i < SHM_MAX_ATTACH; i++)
        if (!g_attaches[i].shmid) {
            slot = &g_attaches[i];
            break;
        }
    if (!slot)
        return (uint64_t) (-(int64_t) ENOMEM);

    uint64_t va;
    if (shmaddr) {
        va = (shmflg & SHM_RND) ? (shmaddr & ~(uint64_t) (PAGE_SIZE - 1)) : shmaddr;
        if (va & (PAGE_SIZE - 1))
            return (uint64_t) (-(int64_t) EINVAL);
    } else {
        uint64_t sz = (uint64_t) s->n_pages * PAGE_SIZE;
        p->mmap_bump += sz;
        va = p->mmap_bump - sz;
    }

    uint64_t vf = VMM_UDATA;
    if (shmflg & SHM_RDONLY)
        vf &= ~(uint64_t) VMM_WRITE;

    for (int i = 0; i < s->n_pages; i++) {
        if (vmm_map(p->space, va + (uint64_t) i * PAGE_SIZE, (uint64_t) s->pages[i], vf) < 0) {
            for (int j = 0; j < i; j++)
                vmm_unmap(p->space, va + (uint64_t) j * PAGE_SIZE);
            return (uint64_t) (-(int64_t) ENOMEM);
        }
    }

    slot->shmid = shmid;
    slot->va = va;
    slot->pid = p->pid;
    slot->n_pages = s->n_pages;
    s->ref_count++;
    return va;
}

int sys_shmdt(uint64_t addr)
{
    proc_t* p = g_current_proc;
    if (!p)
        return -EINVAL;

    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        if (!g_attaches[i].shmid)
            continue;
        if (g_attaches[i].pid != p->pid)
            continue;
        if (g_attaches[i].va != addr)
            continue;

        shm_seg_t* s = seg_by_id(g_attaches[i].shmid);
        if (s) {
            for (int j = 0; j < g_attaches[i].n_pages; j++)
                vmm_unmap(p->space, addr + (uint64_t) j * PAGE_SIZE);
            s->ref_count--;
            if (s->ref_count <= 0 && s->destroy_pending)
                seg_destroy(s);
        }
        g_attaches[i].shmid = 0;
        return 0;
    }
    return -EINVAL;
}

/* 0 + fill key fields */
int sys_shmctl(int shmid, int cmd, void* buf)
{
    shm_seg_t* s = seg_by_id(shmid);
    if (!s)
        return -EINVAL;

    switch (cmd) {
    case IPC_RMID:
        if (s->ref_count > 0)
            s->destroy_pending = 1;
        else
            seg_destroy(s);
        return 0;
    case IPC_STAT:
        if (!buf)
            return -EINVAL;
        memset(buf, 0, 144);                             /* zero out shmid_ds */
        ((uint64_t*) buf)[6] = s->size;                  /* shm_segsz at offset 48 */
        ((uint64_t*) buf)[13] = (uint64_t) s->ref_count; /* shm_nattch */
        return 0;
    default:
        return -EINVAL;
    }
}

void shm_proc_exit(uint32_t pid)
{
    proc_t* p = g_current_proc;
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        if (!g_attaches[i].shmid)
            continue;
        if (g_attaches[i].pid != pid)
            continue;
        shm_seg_t* s = seg_by_id(g_attaches[i].shmid);
        if (s && p && p->space) {
            for (int j = 0; j < g_attaches[i].n_pages; j++)
                vmm_unmap(p->space, g_attaches[i].va + (uint64_t) j * PAGE_SIZE);
            s->ref_count--;
            if (s->ref_count <= 0 && s->destroy_pending)
                seg_destroy(s);
        }
        g_attaches[i].shmid = 0;
    }
}
