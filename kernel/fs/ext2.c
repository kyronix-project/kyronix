#include "ext2.h"
#include "../drivers/block.h"
#include "../lib/log.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "vfs.h"

#define EXT2_SB_LBA 2u
#define EXT2_MAGIC 0xEF53u

#define EXT2_S_IFMT 0xF000u
#define EXT2_S_IFREG 0x8000u
#define EXT2_S_IFDIR 0x4000u
#define EXT2_S_IFLNK 0xA000u

#define EXT2_FT_UNKNOWN 0u
#define EXT2_FT_REG 1u
#define EXT2_FT_DIR 2u
#define EXT2_FT_SLNK 7u

#define EXT2_INCOMPAT_FILETYPE 0x0002u
#define EXT2_RO_COMPAT_LARGE 0x0002u

#define DIRENT_LEN(nlen) (((uint32_t) (nlen) + 8u + 3u) & ~3u)

typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime, s_wtime;
    uint16_t s_mnt_count, s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state, s_errors, s_minor_rev_level;
    uint32_t s_lastcheck, s_checkinterval, s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid, s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t s_uuid[16];
    char s_volume_name[16];
    char s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t s_padding[820];
} ext2_sb_t;

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t bg_reserved[12];
} ext2_bgd_t;

typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime, i_ctime, i_mtime, i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t i_osd2[12];
} ext2_inode_t;

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[255];
} ext2_dirent_t;

static struct block_device *g_dev;
static uint32_t g_block_size;
static uint32_t g_blocks_per_group;
static uint32_t g_inodes_per_group;
static uint32_t g_inode_size;
static uint32_t g_first_data_block;
static uint32_t g_num_groups;
static bool g_has_large_file;
static ext2_bgd_t *g_bgdt = NULL;
static uint8_t *g_indir_buf = NULL;
static ext2_sb_t g_sb;
static char g_mount_point[512];

#define EXT2_META_CACHE_ENTRIES 64u
#define EXT2_META_CACHE_WAYS 4u
#define EXT2_META_CACHE_SETS (EXT2_META_CACHE_ENTRIES / EXT2_META_CACHE_WAYS)

_Static_assert(EXT2_META_CACHE_ENTRIES % EXT2_META_CACHE_WAYS == 0,
               "ext2 metadata cache sets must be whole");

static uint8_t *g_meta_cache = NULL;
static uint32_t g_meta_cache_blocks[EXT2_META_CACHE_ENTRIES];
static uint8_t g_meta_cache_valid[EXT2_META_CACHE_ENTRIES];
static uint8_t g_meta_cache_next[EXT2_META_CACHE_SETS];
static uint32_t *g_block_hints = NULL;
static uint32_t *g_inode_hints = NULL;
static int g_tracked_cnt;

static void clear_mount_state(void) {
    if (g_bgdt) kfree(g_bgdt);
    if (g_indir_buf) kfree(g_indir_buf);
    if (g_meta_cache) kfree(g_meta_cache);
    if (g_block_hints) kfree(g_block_hints);
    if (g_inode_hints) kfree(g_inode_hints);
    g_dev = NULL;
    g_block_size = 0;
    g_blocks_per_group = 0;
    g_inodes_per_group = 0;
    g_inode_size = 0;
    g_first_data_block = 0;
    g_num_groups = 0;
    g_has_large_file = false;
    g_bgdt = NULL;
    g_indir_buf = NULL;
    g_meta_cache = NULL;
    g_block_hints = NULL;
    g_inode_hints = NULL;
    memset(g_meta_cache_valid, 0, sizeof(g_meta_cache_valid));
    memset(g_meta_cache_next, 0, sizeof(g_meta_cache_next));
    memset(&g_sb, 0, sizeof(g_sb));
    g_mount_point[0] = '\0';
    g_tracked_cnt = 0;
}

static void track_node(uint32_t ino_nr, vfs_node_t *n) {
    if (!n || !ino_nr) return;
    n->ino = ino_nr;
    n->fs_private = (void *) (uintptr_t) ino_nr;
    g_tracked_cnt++;
}

static uint32_t tracked_ino(const vfs_node_t *n) {
    return n ? (uint32_t) (uintptr_t) n->fs_private : 0;
}

static int read_block(uint32_t blk, void *buf) {
    uint64_t lba = (uint64_t) blk * (g_block_size / 512u);
    uint32_t secs = g_block_size / 512u;
    return g_dev->ops->read(g_dev, lba, secs, buf);
}

static uint32_t meta_cache_set(uint32_t blk) { return blk % EXT2_META_CACHE_SETS; }

static int meta_cache_find(uint32_t blk) {
    uint32_t first = meta_cache_set(blk) * EXT2_META_CACHE_WAYS;
    for (uint32_t way = 0; way < EXT2_META_CACHE_WAYS; way++) {
        uint32_t slot = first + way;
        if (g_meta_cache_valid[slot] && g_meta_cache_blocks[slot] == blk) return (int) slot;
    }
    return -1;
}

static uint32_t meta_cache_victim(uint32_t blk) {
    uint32_t set = meta_cache_set(blk);
    uint32_t first = set * EXT2_META_CACHE_WAYS;
    for (uint32_t way = 0; way < EXT2_META_CACHE_WAYS; way++) {
        uint32_t slot = first + way;
        if (!g_meta_cache_valid[slot]) return slot;
    }
    uint32_t slot = first + g_meta_cache_next[set];
    g_meta_cache_next[set] = (g_meta_cache_next[set] + 1u) % EXT2_META_CACHE_WAYS;
    return slot;
}

static int write_block(uint32_t blk, const void *buf) {
    uint64_t lba = (uint64_t) blk * (g_block_size / 512u);
    uint32_t secs = g_block_size / 512u;
    int r = g_dev->ops->write(g_dev, lba, secs, buf);
    int slot = meta_cache_find(blk);
    if (r == 0 && slot >= 0) g_meta_cache_valid[slot] = 0;
    return r;
}

static int read_meta_block(uint32_t blk, void *buf) {
    if (!g_meta_cache) return read_block(blk, buf);
    int found = meta_cache_find(blk);
    if (found >= 0) {
        memcpy(buf, g_meta_cache + (uint64_t) found * g_block_size, g_block_size);
        return 0;
    }
    uint32_t slot = meta_cache_victim(blk);
    uint8_t *cached = g_meta_cache + (uint64_t) slot * g_block_size;
    if (read_block(blk, buf) < 0) return -1;
    memcpy(cached, buf, g_block_size);
    g_meta_cache_blocks[slot] = blk;
    g_meta_cache_valid[slot] = 1;
    return 0;
}

static int write_meta_block(uint32_t blk, const void *buf) {
    if (write_block(blk, buf) < 0) return -1;
    if (!g_meta_cache) return 0;
    int found = meta_cache_find(blk);
    uint32_t slot = found >= 0 ? (uint32_t) found : meta_cache_victim(blk);
    memcpy(g_meta_cache + (uint64_t) slot * g_block_size, buf, g_block_size);
    g_meta_cache_blocks[slot] = blk;
    g_meta_cache_valid[slot] = 1;
    return 0;
}

static int write_sb(void) { return g_dev->ops->write(g_dev, EXT2_SB_LBA, 2u, &g_sb); }

static int write_bgd(uint32_t group) {
    uint32_t bgdt_blk = g_first_data_block + 1u;
    uint64_t byte_off = (uint64_t) group * sizeof(ext2_bgd_t);
    uint32_t blk = bgdt_blk + (uint32_t) (byte_off / g_block_size);
    uint32_t off = (uint32_t) (byte_off % g_block_size);
    uint8_t *tmp = (uint8_t *) kmalloc(g_block_size);
    if (!tmp) return -1;
    if (read_meta_block(blk, tmp) < 0) {
        kfree(tmp);
        return -1;
    }
    memcpy(tmp + off, &g_bgdt[group], sizeof(ext2_bgd_t));
    int r = write_meta_block(blk, tmp);
    kfree(tmp);
    return r;
}

static int read_inode(uint32_t ino, ext2_inode_t *out) {
    if (!ino) return -1;
    uint32_t group = (ino - 1u) / g_inodes_per_group;
    if (group >= g_num_groups) return -1;
    uint32_t idx = (ino - 1u) % g_inodes_per_group;
    uint64_t byteoff =
        (uint64_t) g_bgdt[group].bg_inode_table * g_block_size + (uint64_t) idx * g_inode_size;
    uint32_t blk = (uint32_t) (byteoff / g_block_size);
    uint32_t off = (uint32_t) (byteoff % g_block_size);
    uint8_t tmp[4096];
    if (off + sizeof(ext2_inode_t) > g_block_size || read_meta_block(blk, tmp) < 0) return -1;
    memcpy(out, tmp + off, sizeof(ext2_inode_t));
    return 0;
}

static int write_inode(uint32_t ino, const ext2_inode_t *in) {
    if (!ino) return -1;
    uint32_t group = (ino - 1u) / g_inodes_per_group;
    if (group >= g_num_groups) return -1;
    uint32_t idx = (ino - 1u) % g_inodes_per_group;
    uint64_t byteoff =
        (uint64_t) g_bgdt[group].bg_inode_table * g_block_size + (uint64_t) idx * g_inode_size;
    uint32_t blk = (uint32_t) (byteoff / g_block_size);
    uint32_t off = (uint32_t) (byteoff % g_block_size);
    uint8_t tmp[4096];
    if (off + sizeof(ext2_inode_t) > g_block_size || read_meta_block(blk, tmp) < 0) return -1;
    memcpy(tmp + off, in, sizeof(ext2_inode_t));
    return write_meta_block(blk, tmp);
}

static uint32_t alloc_block(uint32_t preferred_group) {
    for (uint32_t gi = 0; gi < g_num_groups; gi++) {
        uint32_t group = (preferred_group + gi) % g_num_groups;
        if (!g_bgdt[group].bg_free_blocks_count) continue;

        uint8_t *bm = (uint8_t *) kmalloc(g_block_size);
        if (!bm) return 0;
        if (read_meta_block(g_bgdt[group].bg_block_bitmap, bm) < 0) {
            kfree(bm);
            continue;
        }

        uint32_t count = g_blocks_per_group;
        if (group == g_num_groups - 1u) {
            uint32_t used = (g_num_groups - 1u) * g_blocks_per_group;
            count = g_sb.s_blocks_count > used ? g_sb.s_blocks_count - used : 0;
        }
        if (!count) {
            kfree(bm);
            continue;
        }

        uint32_t start = g_block_hints ? g_block_hints[group] % count : 0;
        for (uint32_t scan = 0; scan < count; scan++) {
            uint32_t bi = (start + scan) % count;
            if (bm[bi / 8u] & (1u << (bi % 8u))) continue;
            bm[bi / 8u] |= (1u << (bi % 8u));
            if (write_meta_block(g_bgdt[group].bg_block_bitmap, bm) < 0) {
                kfree(bm);
                return 0;
            }
            kfree(bm);
            if (g_block_hints) g_block_hints[group] = bi + 1u;

            if (g_bgdt[group].bg_free_blocks_count) g_bgdt[group].bg_free_blocks_count--;
            write_bgd(group);
            if (g_sb.s_free_blocks_count) g_sb.s_free_blocks_count--;
            write_sb();

            uint32_t abs_blk = group * g_blocks_per_group + bi;
            uint8_t *z = (uint8_t *) kmalloc(g_block_size);
            if (z) {
                memset(z, 0, g_block_size);
                write_block(abs_blk, z);
                kfree(z);
            }
            return abs_blk;
        }
        kfree(bm);
    }
    return 0;
}

static void free_block(uint32_t blk_nr) {
    if (!blk_nr) return;
    uint32_t group = blk_nr / g_blocks_per_group;
    uint32_t bi = blk_nr % g_blocks_per_group;
    if (group >= g_num_groups) return;

    uint8_t *bm = (uint8_t *) kmalloc(g_block_size);
    if (!bm) return;
    if (read_meta_block(g_bgdt[group].bg_block_bitmap, bm) < 0) {
        kfree(bm);
        return;
    }
    bm[bi / 8u] &= ~(uint8_t) (1u << (bi % 8u));
    write_meta_block(g_bgdt[group].bg_block_bitmap, bm);
    kfree(bm);
    if (g_block_hints && bi < g_block_hints[group]) g_block_hints[group] = bi;

    g_bgdt[group].bg_free_blocks_count++;
    write_bgd(group);
    g_sb.s_free_blocks_count++;
    write_sb();
}

static void free_indirect(uint32_t blk, int depth) {
    if (!blk) return;
    uint32_t *buf = (uint32_t *) kmalloc(g_block_size);
    if (buf) {
        if (read_meta_block(blk, buf) == 0) {
            uint32_t ptrs = g_block_size / 4u;
            for (uint32_t i = 0; i < ptrs; i++) {
                if (!buf[i]) continue;
                if (depth == 1)
                    free_block(buf[i]);
                else
                    free_indirect(buf[i], depth - 1);
            }
        }
        kfree(buf);
    }
    free_block(blk);
}

static void free_all_blocks(ext2_inode_t *ino) {
    for (int i = 0; i < 12; i++) {
        if (ino->i_block[i]) {
            free_block(ino->i_block[i]);
            ino->i_block[i] = 0;
        }
    }
    free_indirect(ino->i_block[12], 1);
    ino->i_block[12] = 0;
    free_indirect(ino->i_block[13], 2);
    ino->i_block[13] = 0;
    free_indirect(ino->i_block[14], 3);
    ino->i_block[14] = 0;
    ino->i_blocks = 0;
}

static uint32_t alloc_inode(uint32_t preferred_group) {
    for (uint32_t gi = 0; gi < g_num_groups; gi++) {
        uint32_t group = (preferred_group + gi) % g_num_groups;
        if (!g_bgdt[group].bg_free_inodes_count) continue;

        uint8_t *bm = (uint8_t *) kmalloc(g_block_size);
        if (!bm) return 0;
        if (read_meta_block(g_bgdt[group].bg_inode_bitmap, bm) < 0) {
            kfree(bm);
            continue;
        }

        uint32_t start = g_inode_hints ? g_inode_hints[group] % g_inodes_per_group : 0;
        for (uint32_t scan = 0; scan < g_inodes_per_group; scan++) {
            uint32_t bi = (start + scan) % g_inodes_per_group;
            if (bm[bi / 8u] & (1u << (bi % 8u))) continue;
            bm[bi / 8u] |= (1u << (bi % 8u));
            if (write_meta_block(g_bgdt[group].bg_inode_bitmap, bm) < 0) {
                kfree(bm);
                return 0;
            }
            kfree(bm);
            if (g_inode_hints) g_inode_hints[group] = bi + 1u;

            if (g_bgdt[group].bg_free_inodes_count) g_bgdt[group].bg_free_inodes_count--;
            write_bgd(group);
            if (g_sb.s_free_inodes_count) g_sb.s_free_inodes_count--;
            write_sb();

            return group * g_inodes_per_group + bi + 1u;
        }
        kfree(bm);
    }
    return 0;
}

static void free_inode(uint32_t ino_nr) {
    if (!ino_nr) return;
    uint32_t group = (ino_nr - 1u) / g_inodes_per_group;
    uint32_t bi = (ino_nr - 1u) % g_inodes_per_group;
    if (group >= g_num_groups) return;

    uint8_t *bm = (uint8_t *) kmalloc(g_block_size);
    if (!bm) return;
    if (read_meta_block(g_bgdt[group].bg_inode_bitmap, bm) < 0) {
        kfree(bm);
        return;
    }
    bm[bi / 8u] &= ~(uint8_t) (1u << (bi % 8u));
    write_meta_block(g_bgdt[group].bg_inode_bitmap, bm);
    kfree(bm);
    if (g_inode_hints && bi < g_inode_hints[group]) g_inode_hints[group] = bi;

    g_bgdt[group].bg_free_inodes_count++;
    write_bgd(group);
    g_sb.s_free_inodes_count++;
    write_sb();
}

static uint32_t indirect_lookup(uint32_t blk, uint32_t idx) {
    if (!blk) return 0;
    if (read_meta_block(blk, g_indir_buf) < 0) return 0;
    return ((uint32_t *) g_indir_buf)[idx];
}

static uint32_t get_block_nr(const ext2_inode_t *ino, uint32_t bi) {
    uint32_t ptrs = g_block_size / 4u;

    if (bi < 12u) return ino->i_block[bi];

    bi -= 12u;
    if (bi < ptrs) return indirect_lookup(ino->i_block[12], bi);

    bi -= ptrs;
    if (bi < ptrs * ptrs) {
        uint32_t l1 = indirect_lookup(ino->i_block[13], bi / ptrs);
        return indirect_lookup(l1, bi % ptrs);
    }

    bi -= ptrs * ptrs;
    {
        uint32_t l1 = indirect_lookup(ino->i_block[14], bi / (ptrs * ptrs));
        uint32_t l2 = indirect_lookup(l1, (bi / ptrs) % ptrs);
        return indirect_lookup(l2, bi % ptrs);
    }
}

static int set_block_ptr(ext2_inode_t *ino, uint32_t ino_nr, uint32_t bi, uint32_t blk_nr) {
    uint32_t ptrs = g_block_size / 4u;
    uint32_t group = (ino_nr - 1u) / g_inodes_per_group;

    if (bi < 12u) {
        ino->i_block[bi] = blk_nr;
        return 0;
    }
    bi -= 12u;

    if (bi < ptrs) {
        if (!ino->i_block[12]) {
            ino->i_block[12] = alloc_block(group);
            if (!ino->i_block[12]) return -1;
        }
        uint32_t *buf = (uint32_t *) kmalloc(g_block_size);
        if (!buf) return -1;
        if (read_meta_block(ino->i_block[12], buf) < 0) {
            kfree(buf);
            return -1;
        }
        buf[bi] = blk_nr;
        int r = write_meta_block(ino->i_block[12], buf);
        kfree(buf);
        return r;
    }
    bi -= ptrs;

    if (bi < ptrs * ptrs) {
        uint32_t l1i = bi / ptrs, l2i = bi % ptrs;
        if (!ino->i_block[13]) {
            ino->i_block[13] = alloc_block(group);
            if (!ino->i_block[13]) return -1;
        }
        uint32_t *l1 = (uint32_t *) kmalloc(g_block_size);
        if (!l1) return -1;
        if (read_meta_block(ino->i_block[13], l1) < 0) {
            kfree(l1);
            return -1;
        }
        if (!l1[l1i]) {
            l1[l1i] = alloc_block(group);
            if (!l1[l1i]) {
                kfree(l1);
                return -1;
            }
            write_meta_block(ino->i_block[13], l1);
        }
        uint32_t *l2 = (uint32_t *) kmalloc(g_block_size);
        if (!l2) {
            kfree(l1);
            return -1;
        }
        if (read_meta_block(l1[l1i], l2) < 0) {
            kfree(l2);
            kfree(l1);
            return -1;
        }
        l2[l2i] = blk_nr;
        int r = write_meta_block(l1[l1i], l2);
        kfree(l2);
        kfree(l1);
        return r;
    }
    bi -= ptrs * ptrs;

    {
        uint32_t l1i = bi / (ptrs * ptrs);
        uint32_t l2i = (bi / ptrs) % ptrs;
        uint32_t l3i = bi % ptrs;
        if (!ino->i_block[14]) {
            ino->i_block[14] = alloc_block(group);
            if (!ino->i_block[14]) return -1;
        }
        uint32_t *l1 = (uint32_t *) kmalloc(g_block_size);
        if (!l1) return -1;
        if (read_meta_block(ino->i_block[14], l1) < 0) {
            kfree(l1);
            return -1;
        }
        if (!l1[l1i]) {
            l1[l1i] = alloc_block(group);
            if (!l1[l1i]) {
                kfree(l1);
                return -1;
            }
            write_meta_block(ino->i_block[14], l1);
        }
        uint32_t *l2 = (uint32_t *) kmalloc(g_block_size);
        if (!l2) {
            kfree(l1);
            return -1;
        }
        if (read_meta_block(l1[l1i], l2) < 0) {
            kfree(l2);
            kfree(l1);
            return -1;
        }
        if (!l2[l2i]) {
            l2[l2i] = alloc_block(group);
            if (!l2[l2i]) {
                kfree(l2);
                kfree(l1);
                return -1;
            }
            write_meta_block(l1[l1i], l2);
        }
        uint32_t *l3 = (uint32_t *) kmalloc(g_block_size);
        if (!l3) {
            kfree(l2);
            kfree(l1);
            return -1;
        }
        if (read_meta_block(l2[l2i], l3) < 0) {
            kfree(l3);
            kfree(l2);
            kfree(l1);
            return -1;
        }
        l3[l3i] = blk_nr;
        int r = write_meta_block(l2[l2i], l3);
        kfree(l3);
        kfree(l2);
        kfree(l1);
        return r;
    }
}

static uint64_t inode_size_64(const ext2_inode_t *ino) {
    uint64_t sz = ino->i_size;
    if (g_has_large_file && (ino->i_mode & EXT2_S_IFMT) == EXT2_S_IFREG)
        sz |= ((uint64_t) ino->i_dir_acl << 32);
    return sz;
}

static uint8_t *read_file_data(const ext2_inode_t *ino, uint64_t *size_out) {
    uint64_t size = inode_size_64(ino);
    *size_out = size;
    if (!size) return NULL;

    uint8_t *data = (uint8_t *) kmalloc(size);
    if (!data) {
        log_warn("ext2: kmalloc %lu bytes failed", size);
        *size_out = 0;
        return NULL;
    }

    uint8_t *blkbuf = (uint8_t *) kmalloc(g_block_size);
    if (!blkbuf) {
        kfree(data);
        *size_out = 0;
        return NULL;
    }

    uint64_t copied = 0;
    for (uint32_t bi = 0; copied < size; bi++) {
        uint32_t blkn = get_block_nr(ino, bi);
        uint64_t tocopy = size - copied;
        if (tocopy > g_block_size) tocopy = g_block_size;

        if (blkn) {
            if (read_block(blkn, blkbuf) < 0) {
                kfree(blkbuf);
                kfree(data);
                return NULL;
            }
            memcpy(data + copied, blkbuf, (size_t) tocopy);
        } else {
            memset(data + copied, 0, (size_t) tocopy);
        }
        copied += tocopy;
    }
    kfree(blkbuf);
    return data;
}

static int read_file_range(const ext2_inode_t *ino, uint8_t *dst, uint64_t off, uint64_t len) {
    uint8_t *blkbuf = (uint8_t *) kmalloc(g_block_size);
    if (!blkbuf) return -1;

    while (len) {
        uint32_t bi = (uint32_t) (off / g_block_size);
        uint32_t block_off = (uint32_t) (off % g_block_size);
        uint64_t chunk = g_block_size - block_off;
        if (chunk > len) chunk = len;

        uint32_t blkn = get_block_nr(ino, bi);
        if (!blkn) {
            memset(dst, 0, (size_t) chunk);
        } else {
            if (read_block(blkn, blkbuf) < 0) {
                kfree(blkbuf);
                return -1;
            }
            memcpy(dst, blkbuf + block_off, (size_t) chunk);
        }

        dst += chunk;
        off += chunk;
        len -= chunk;
    }

    kfree(blkbuf);
    return 0;
}

static uint32_t indirect_blocks_for_data(uint32_t data_blocks) {
    uint64_t ptrs = g_block_size / sizeof(uint32_t);
    uint64_t remaining = data_blocks;
    uint64_t metadata = 0;

    if (remaining <= 12u) return 0;
    remaining -= 12u;

    metadata++; /* single-indirect root */
    uint64_t used = remaining < ptrs ? remaining : ptrs;
    remaining -= used;

    if (remaining) {
        metadata++; /* double-indirect root */
        uint64_t capacity = ptrs * ptrs;
        used = remaining < capacity ? remaining : capacity;
        metadata += (used + ptrs - 1u) / ptrs;
        remaining -= used;
    }

    if (remaining) {
        metadata++; /* triple-indirect root */
        uint64_t double_capacity = ptrs * ptrs;
        metadata += (remaining + double_capacity - 1u) / double_capacity;
        metadata += (remaining + ptrs - 1u) / ptrs;
    }

    return (uint32_t) metadata;
}

int ext2_write_file(uint32_t ino_nr, const void *data, uint64_t size) {
    if (!ino_nr) return -1;
    ext2_inode_t ino;
    if (read_inode(ino_nr, &ino) < 0) return -1;

    uint64_t old_size = inode_size_64(&ino);
    uint32_t old_blocks = (uint32_t) ((old_size + g_block_size - 1u) / g_block_size);
    uint32_t new_blocks = (uint32_t) ((size + g_block_size - 1u) / g_block_size);

    if (new_blocks < old_blocks) {
        free_all_blocks(&ino);
        ino.i_size = 0;
        ino.i_blocks = 0;
        if (g_has_large_file && (ino.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG) ino.i_dir_acl = 0;
        if (write_inode(ino_nr, &ino) < 0) return -1;
        if (!size) return 0;
    }

    uint32_t preferred = (ino_nr - 1u) / g_inodes_per_group;
    uint8_t *blkbuf = (uint8_t *) kmalloc(g_block_size);
    const uint8_t *src = (const uint8_t *) data;
    if (!blkbuf) return -1;

    for (uint32_t bi = 0; bi < new_blocks; bi++) {
        uint32_t blkn = get_block_nr(&ino, bi);
        if (!blkn) {
            blkn = alloc_block(preferred);
            if (!blkn) {
                kfree(blkbuf);
                return -1;
            }
            if (set_block_ptr(&ino, ino_nr, bi, blkn) < 0) {
                free_block(blkn);
                kfree(blkbuf);
                return -1;
            }
        }
        uint64_t off = (uint64_t) bi * g_block_size;
        uint64_t tocopy = size - off;
        if (tocopy > g_block_size) tocopy = g_block_size;
        memset(blkbuf, 0, g_block_size);
        if (data) memcpy(blkbuf, src + off, (size_t) tocopy);
        if (write_block(blkn, blkbuf) < 0) {
            kfree(blkbuf);
            return -1;
        }
    }
    kfree(blkbuf);

    ino.i_size = (uint32_t) (size & 0xFFFFFFFFu);
    ino.i_blocks =
        (new_blocks + indirect_blocks_for_data(new_blocks)) *
        (g_block_size / 512u);
    if (g_has_large_file && (ino.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG)
        ino.i_dir_acl = (uint32_t) (size >> 32);

    return write_inode(ino_nr, &ino);
}

static uint32_t ext2_lookup(uint32_t dir_ino, const char *name) {
    uint8_t name_len = (uint8_t) strlen(name);
    ext2_inode_t diri;
    if (read_inode(dir_ino, &diri) < 0) return 0;

    uint32_t nblocks = (uint32_t) ((inode_size_64(&diri) + g_block_size - 1u) / g_block_size);
    uint8_t *blk = (uint8_t *) kmalloc(g_block_size);
    if (!blk) return 0;

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t blkn = get_block_nr(&diri, bi);
        if (!blkn || read_block(blkn, blk) < 0) continue;
        uint32_t pos = 0;
        while (pos + 8u <= g_block_size) {
            const ext2_dirent_t *de = (const ext2_dirent_t *) (blk + pos);
            if (!de->rec_len || de->rec_len < 8u) break;
            if (de->inode && de->name_len == name_len && memcmp(de->name, name, name_len) == 0) {
                uint32_t r = de->inode;
                kfree(blk);
                return r;
            }
            pos += de->rec_len;
        }
    }
    kfree(blk);
    return 0;
}

static int ext2_add_dirent(uint32_t dir_ino, const char *name, uint32_t new_ino, uint8_t ftype) {
    uint8_t name_len = (uint8_t) strlen(name);
    uint32_t needed = DIRENT_LEN(name_len);

    ext2_inode_t diri;
    if (read_inode(dir_ino, &diri) < 0) return -1;

    uint64_t dir_size = inode_size_64(&diri);
    uint32_t nblocks = (uint32_t) ((dir_size + g_block_size - 1u) / g_block_size);

    uint8_t *blk = (uint8_t *) kmalloc(g_block_size);
    if (!blk) return -1;

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t blkn = get_block_nr(&diri, bi);
        if (!blkn || read_block(blkn, blk) < 0) continue;

        uint32_t pos = 0;
        while (pos + 8u <= g_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *) (blk + pos);
            if (!de->rec_len || de->rec_len < 8u) break;

            uint32_t actual = de->inode ? DIRENT_LEN(de->name_len) : 0u;
            uint32_t slack = de->rec_len - actual;

            if (slack >= needed) {
                if (actual) {
                    uint16_t old_rec = de->rec_len;
                    de->rec_len = (uint16_t) actual;
                    de = (ext2_dirent_t *) (blk + pos + actual);
                    de->rec_len = (uint16_t) (old_rec - actual);
                }
                de->inode = new_ino;
                de->name_len = name_len;
                de->file_type = ftype;
                memcpy(de->name, name, name_len);
                write_block(blkn, blk);
                kfree(blk);
                return 0;
            }
            pos += de->rec_len;
        }
    }

    uint32_t group = (dir_ino - 1u) / g_inodes_per_group;
    uint32_t new_blkn = alloc_block(group);
    if (!new_blkn) {
        kfree(blk);
        return -1;
    }

    memset(blk, 0, g_block_size);
    ext2_dirent_t *de = (ext2_dirent_t *) blk;
    de->inode = new_ino;
    de->rec_len = (uint16_t) g_block_size;
    de->name_len = name_len;
    de->file_type = ftype;
    memcpy(de->name, name, name_len);
    write_block(new_blkn, blk);

    if (set_block_ptr(&diri, dir_ino, nblocks, new_blkn) < 0) {
        free_block(new_blkn);
        kfree(blk);
        return -1;
    }
    diri.i_size = (uint32_t) ((uint64_t) (nblocks + 1u) * g_block_size);
    diri.i_blocks += g_block_size / 512u;
    write_inode(dir_ino, &diri);

    kfree(blk);
    return 0;
}

static int ext2_remove_dirent(uint32_t dir_ino, const char *name, uint32_t *ino_out) {
    uint8_t name_len = (uint8_t) strlen(name);
    ext2_inode_t diri;
    if (read_inode(dir_ino, &diri) < 0) return -1;

    uint32_t nblocks = (uint32_t) ((inode_size_64(&diri) + g_block_size - 1u) / g_block_size);
    uint8_t *blk = (uint8_t *) kmalloc(g_block_size);
    if (!blk) return -1;

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t blkn = get_block_nr(&diri, bi);
        if (!blkn || read_block(blkn, blk) < 0) continue;

        uint32_t pos = 0;
        ext2_dirent_t *prev = NULL;

        while (pos + 8u <= g_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *) (blk + pos);
            if (!de->rec_len || de->rec_len < 8u) break;

            if (de->inode && de->name_len == name_len && memcmp(de->name, name, name_len) == 0) {
                if (ino_out) *ino_out = de->inode;
                if (prev)
                    prev->rec_len += de->rec_len;
                else
                    de->inode = 0;
                write_block(blkn, blk);
                kfree(blk);
                return 0;
            }
            prev = de;
            pos += de->rec_len;
        }
    }
    kfree(blk);
    return -1;
}

static const char *strip_mount(const char *path) {
    size_t mplen = strlen(g_mount_point);
    if (strncmp(path, g_mount_point, mplen) == 0) path += mplen;
    while (*path == '/') path++;
    return path;
}

static int resolve_parent(const char *path, uint32_t *parent_ino, const char **childname) {
    const char *rel = strip_mount(path);

    const char *last_slash = NULL;
    for (const char *p = rel; *p; p++)
        if (*p == '/') last_slash = p;

    if (!last_slash) {
        *parent_ino = 2u;
        *childname = rel;
        return 0;
    }
    *childname = last_slash + 1;

    uint32_t cur = 2u;
    const char *p = rel;
    while (p < last_slash) {
        while (p < last_slash && *p == '/') p++;
        const char *end = p;
        while (end < last_slash && *end != '/') end++;
        if (end == p) continue;

        char comp[256];
        size_t len = (size_t) (end - p);
        if (len >= 256) return -1;
        memcpy(comp, p, len);
        comp[len] = '\0';
        cur = ext2_lookup(cur, comp);
        if (!cur) return -1;
        p = end;
    }
    *parent_ino = cur;
    return 0;
}

int ext2_create(const char *path, uint16_t mode, const void *data, uint64_t size) {
    uint32_t parent_ino;
    const char *name;
    if (resolve_parent(path, &parent_ino, &name) < 0 || !*name) return -1;
    if (ext2_lookup(parent_ino, name)) return -1;

    uint32_t group = (parent_ino - 1u) / g_inodes_per_group;
    uint32_t ino_nr = alloc_inode(group);
    if (!ino_nr) return -1;

    ext2_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_mode = EXT2_S_IFREG | (mode & 07777u);
    ino.i_links_count = 1;
    if (write_inode(ino_nr, &ino) < 0) {
        free_inode(ino_nr);
        return -1;
    }

    if (data && size && ext2_write_file(ino_nr, data, size) < 0) {
        free_inode(ino_nr);
        return -1;
    }

    if (ext2_add_dirent(parent_ino, name, ino_nr, EXT2_FT_REG) < 0) {
        ext2_inode_t tmp;
        if (read_inode(ino_nr, &tmp) == 0) {
            free_all_blocks(&tmp);
            write_inode(ino_nr, &tmp);
        }
        free_inode(ino_nr);
        return -1;
    }
    return 0;
}

int ext2_mkdir(const char *path, uint16_t mode) {
    uint32_t parent_ino;
    const char *name;
    if (resolve_parent(path, &parent_ino, &name) < 0 || !*name) return -1;
    if (ext2_lookup(parent_ino, name)) return -1;

    uint32_t group = (parent_ino - 1u) / g_inodes_per_group;
    uint32_t ino_nr = alloc_inode(group);
    if (!ino_nr) return -1;

    ext2_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_mode = EXT2_S_IFDIR | (mode & 07777u);
    ino.i_links_count = 2;
    if (write_inode(ino_nr, &ino) < 0) {
        free_inode(ino_nr);
        return -1;
    }

    if (ext2_add_dirent(ino_nr, ".", ino_nr, EXT2_FT_DIR) < 0 ||
        ext2_add_dirent(ino_nr, "..", parent_ino, EXT2_FT_DIR) < 0 ||
        ext2_add_dirent(parent_ino, name, ino_nr, EXT2_FT_DIR) < 0) {
        free_inode(ino_nr);
        return -1;
    }

    ext2_inode_t pino;
    if (read_inode(parent_ino, &pino) == 0) {
        pino.i_links_count++;
        write_inode(parent_ino, &pino);
    }
    uint32_t pg = (ino_nr - 1u) / g_inodes_per_group;
    g_bgdt[pg].bg_used_dirs_count++;
    write_bgd(pg);
    return 0;
}

int ext2_unlink(const char *path) {
    uint32_t parent_ino;
    const char *name;
    if (resolve_parent(path, &parent_ino, &name) < 0 || !*name) return -1;

    uint32_t ino_nr = 0;
    if (ext2_remove_dirent(parent_ino, name, &ino_nr) < 0 || !ino_nr) return -1;

    ext2_inode_t ino;
    if (read_inode(ino_nr, &ino) < 0) return 0;

    if (ino.i_links_count > 1) {
        ino.i_links_count--;
        return write_inode(ino_nr, &ino);
    }

    bool is_dir = (ino.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
    free_all_blocks(&ino);
    ino.i_links_count = 0;
    ino.i_dtime = 0;
    write_inode(ino_nr, &ino);
    free_inode(ino_nr);

    if (is_dir) {
        ext2_inode_t pino;
        if (read_inode(parent_ino, &pino) == 0 && pino.i_links_count > 1) {
            pino.i_links_count--;
            write_inode(parent_ino, &pino);
        }
        uint32_t pg = (ino_nr - 1u) / g_inodes_per_group;
        if (g_bgdt[pg].bg_used_dirs_count) g_bgdt[pg].bg_used_dirs_count--;
        write_bgd(pg);
    }
    return 0;
}

int ext2_symlink(const char *path, const char *target) {
    uint32_t parent_ino;
    const char *name;
    if (resolve_parent(path, &parent_ino, &name) < 0 || !*name) return -1;
    if (ext2_lookup(parent_ino, name)) return -1;

    uint32_t group = (parent_ino - 1u) / g_inodes_per_group;
    uint32_t ino_nr = alloc_inode(group);
    if (!ino_nr) return -1;

    ext2_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_mode = EXT2_S_IFLNK | 0777u;
    ino.i_links_count = 1;

    size_t tlen = strlen(target);
    if (tlen < 60u) {
        memcpy(ino.i_block, target, tlen);
        ino.i_size = (uint32_t) tlen;
        if (write_inode(ino_nr, &ino) < 0) {
            free_inode(ino_nr);
            return -1;
        }
    } else {
        if (write_inode(ino_nr, &ino) < 0) {
            free_inode(ino_nr);
            return -1;
        }
        if (ext2_write_file(ino_nr, target, tlen) < 0) {
            free_inode(ino_nr);
            return -1;
        }
    }

    if (ext2_add_dirent(parent_ino, name, ino_nr, EXT2_FT_SLNK) < 0) {
        ext2_inode_t tmp;
        if (read_inode(ino_nr, &tmp) == 0) {
            free_all_blocks(&tmp);
            write_inode(ino_nr, &tmp);
        }
        free_inode(ino_nr);
        return -1;
    }
    return 0;
}

static uint32_t create_disk_node(vfs_node_t *node, uint32_t parent_ino) {
    uint32_t group = (parent_ino - 1u) / g_inodes_per_group;
    uint32_t new_ino = alloc_inode(group);
    if (!new_ino) return 0;

    ext2_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_uid = node->uid;
    ino.i_gid = node->gid;

    if (node->type == VFS_TYPE_REG) {
        ino.i_mode = EXT2_S_IFREG | (node->mode & 07777u);
        ino.i_links_count = 1;
        write_inode(new_ino, &ino);
        ext2_write_file(new_ino, node->data, node->size);
        ext2_add_dirent(parent_ino, node->name, new_ino, EXT2_FT_REG);
        track_node(new_ino, node);
        return new_ino;
    }
    if (node->type == VFS_TYPE_DIR) {
        ino.i_mode = EXT2_S_IFDIR | (node->mode & 07777u);
        ino.i_links_count = 2;
        write_inode(new_ino, &ino);
        ext2_add_dirent(new_ino, ".", new_ino, EXT2_FT_DIR);
        ext2_add_dirent(new_ino, "..", parent_ino, EXT2_FT_DIR);
        ext2_add_dirent(parent_ino, node->name, new_ino, EXT2_FT_DIR);
        ext2_inode_t parent;
        if (read_inode(parent_ino, &parent) == 0) {
            parent.i_links_count++;
            write_inode(parent_ino, &parent);
        }
        uint32_t dir_group = (new_ino - 1u) / g_inodes_per_group;
        g_bgdt[dir_group].bg_used_dirs_count++;
        write_bgd(dir_group);
        track_node(new_ino, node);
        return new_ino;
    }
    if (node->type == VFS_TYPE_SYM && node->symlink) {
        size_t slen = strlen(node->symlink);
        ino.i_mode = EXT2_S_IFLNK | 0777u;
        ino.i_links_count = 1;
        if (slen < 60u) {
            memcpy(ino.i_block, node->symlink, slen);
            ino.i_size = (uint32_t) slen;
            write_inode(new_ino, &ino);
        } else {
            write_inode(new_ino, &ino);
            ext2_write_file(new_ino, node->symlink, slen);
        }
        ext2_add_dirent(parent_ino, node->name, new_ino, EXT2_FT_SLNK);
        track_node(new_ino, node);
        return new_ino;
    }

    free_inode(new_ino);
    return 0;
}

/* ---- fs_ops for demand-loaded regular files ---- */

static int64_t ext2_reg_read(vfs_node_t *n, char *buf, uint64_t off, uint64_t len) {
    if (!n->data && !buf && n->size > 0) {
        uint32_t ino_nr = (uint32_t) (uintptr_t) n->fs_private;
        ext2_inode_t ino;
        if (read_inode(ino_nr, &ino) < 0) return -(int64_t) 5;
        uint64_t size;
        uint8_t *data = read_file_data(&ino, &size);
        if (!data) return -(int64_t) 12;
        n->data = data;
        n->capacity = size;
        n->size = size;
    }
    if (off >= n->size) return 0;
    uint64_t avail = n->size - off;
    uint64_t r = (len < avail) ? len : avail;
    if (!buf || !r) return (int64_t) r;
    if (n->data) {
        memcpy(buf, n->data + off, r);
        return (int64_t) r;
    }

    uint32_t ino_nr = tracked_ino(n);
    ext2_inode_t ino;
    if (!ino_nr || read_inode(ino_nr, &ino) < 0) return -(int64_t) 5;
    if (read_file_range(&ino, (uint8_t *) buf, off, r) < 0) return -(int64_t) 5;
    return (int64_t) r;
}

static int64_t ext2_reg_write(vfs_node_t *n, const char *buf, uint64_t off, uint64_t len) {
    if (!n->data) ext2_reg_read(n, NULL, 0, 0);
    if (off > UINT64_MAX - len) return -(int64_t) 27;
    uint64_t needed = off + len;
    if (needed > n->capacity) {
        uint64_t newcap = n->capacity ? n->capacity * 2 : 512;
        if (newcap < n->capacity) newcap = needed;
        while (newcap < needed) {
            if (newcap > UINT64_MAX / 2) {
                newcap = needed;
                break;
            }
            newcap *= 2;
        }
        uint8_t *newdata = krealloc(n->data, newcap);
        if (!newdata) return -(int64_t) 12;
        n->data = newdata;
        n->capacity = newcap;
    }
    if (needed > n->size) {
        memset(n->data + n->size, 0, needed - n->size);
        n->size = needed;
    }
    memcpy(n->data + off, buf, len);
    n->dirty = 1;
    return (int64_t) len;
}

static void ext2_reg_close(vfs_node_t *n) { (void) n; }

static struct vfs_fs_ops ext2_reg_ops = {
    .read = ext2_reg_read,
    .write = ext2_reg_write,
    .close = ext2_reg_close,
};

/* ---- sync ---- */

static void sync_walk(vfs_node_t *node, uint32_t parent_ext2_ino) {
    if (!node || node->deleted) return;
    if (node->name[0] == '\0' || (node->name[0] == '.' && node->name[1] == '\0') ||
        (node->name[0] == '.' && node->name[1] == '.' && node->name[2] == '\0'))
        return;

    uint32_t ext2_ino = tracked_ino(node);

    if (ext2_ino) {
        if (node->type == VFS_TYPE_REG && node->dirty) {
            ext2_write_file(ext2_ino, node->data, node->size);
            log_info("ext2: sync  wrote ino=%u  size=%lu", ext2_ino, node->size);
            node->dirty = 0;
        }
    } else if (parent_ext2_ino) {
        ext2_ino = create_disk_node(node, parent_ext2_ino);
        if (ext2_ino) log_info("ext2: sync  created ino=%u  name=%s", ext2_ino, node->name);
    }

    if (node->type == VFS_TYPE_DIR && ext2_ino)
        for (vfs_node_t *ch = node->children; ch; ch = ch->next) sync_walk(ch, ext2_ino);
}

int ext2_sync(void) {
    if (!g_dev || !g_bgdt || !g_indir_buf || !g_mount_point[0]) return 0;
    vfs_node_t *root = vfs_lookup(g_mount_point);
    if (!root) {
        log_warn("ext2: sync: mount point %s not found in VFS", g_mount_point);
        return -1;
    }

    int count = 0;
    for (vfs_node_t *ch = root->children; ch; ch = ch->next) {
        sync_walk(ch, 2u);
        count++;
    }

    if (g_dev->ops->flush) g_dev->ops->flush(g_dev);
    log_info("ext2: sync done  (%d children)", count);
    return 0;
}

static void walk_dir(uint32_t dir_ino, const char *dir_path);

static void process_entry(uint32_t ino_nr, const char *path) {
    ext2_inode_t ino;
    if (read_inode(ino_nr, &ino) < 0) return;

    uint16_t type = ino.i_mode & EXT2_S_IFMT;

    if (type == EXT2_S_IFDIR) {
        vfs_node_t *n = vfs_mkdir_p(path, ino.i_mode & 07777u);
        if (n) {
            n->uid = ino.i_uid;
            n->gid = ino.i_gid;
            track_node(ino_nr, n);
        }
        walk_dir(ino_nr, path);
    } else if (type == EXT2_S_IFREG) {
        vfs_node_t *n = vfs_create_file(path, ino.i_mode & 07777u, NULL, 0);
        if (n) {
            n->uid = ino.i_uid;
            n->gid = ino.i_gid;
            n->size = inode_size_64(&ino);
            n->fs_ops = &ext2_reg_ops;
            n->fs_private = (void *) (uintptr_t) ino_nr;
            track_node(ino_nr, n);
        }
    } else if (type == EXT2_S_IFLNK) {
        vfs_node_t *n = NULL;
        if (ino.i_size < 60u) {
            char target[61];
            memcpy(target, ino.i_block, ino.i_size);
            target[ino.i_size] = '\0';
            n = vfs_create_symlink(path, target);
        } else if (ino.i_size >= 60u) {
            uint64_t size;
            uint8_t *data = read_file_data(&ino, &size);
            if (data) {
                char *target = (char *) kmalloc(size + 1u);
                if (target) {
                    memcpy(target, data, size);
                    target[size] = '\0';
                    n = vfs_create_symlink(path, target);
                    kfree(target);
                }
                kfree(data);
            }
        }
        if (n) {
            n->uid = ino.i_uid;
            n->gid = ino.i_gid;
            track_node(ino_nr, n);
        }
    }
}

static void walk_dir(uint32_t dir_ino, const char *dir_path) {
    ext2_inode_t ino;
    if (read_inode(dir_ino, &ino) < 0) return;

    uint32_t nblocks = (uint32_t) ((inode_size_64(&ino) + g_block_size - 1u) / g_block_size);

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t blkn = get_block_nr(&ino, bi);
        if (!blkn) continue;

        uint8_t *blk = (uint8_t *) kmalloc(g_block_size);
        if (!blk) return;
        if (read_block(blkn, blk) < 0) {
            kfree(blk);
            continue;
        }

        uint32_t pos = 0;
        while (pos + 8u <= g_block_size) {
            const ext2_dirent_t *de = (const ext2_dirent_t *) (blk + pos);
            if (!de->rec_len || de->rec_len < 8u) break;

            if (de->inode && de->name_len) {
                bool dot = de->name_len == 1u && de->name[0] == '.';
                bool dotdot = de->name_len == 2u && de->name[0] == '.' && de->name[1] == '.';

                if (!dot && !dotdot) {
                    char name[256];
                    memcpy(name, de->name, de->name_len);
                    name[de->name_len] = '\0';

                    char childpath[512];
                    size_t dlen = strlen(dir_path);
                    if (dlen > 0 && dir_path[dlen - 1u] == '/')
                        snprintf(childpath, sizeof(childpath), "%s%s", dir_path, name);
                    else
                        snprintf(childpath, sizeof(childpath), "%s/%s", dir_path, name);

                    process_entry(de->inode, childpath);
                }
            }
            pos += de->rec_len;
        }
        kfree(blk);
    }
}

bool ext2_check_root(struct block_device *dev) {
    uint8_t sb_buf[1024];
    if (dev->ops->read(dev, EXT2_SB_LBA, 2u, sb_buf) < 0) return false;

    ext2_sb_t *sb = (ext2_sb_t *) sb_buf;
    if (sb->s_magic != EXT2_MAGIC) return false;

    uint32_t block_size = 1024u << sb->s_log_block_size;
    uint32_t inode_size = (sb->s_rev_level >= 1u) ? (uint32_t) sb->s_inode_size : 128u;
    uint32_t first_data_block = sb->s_first_data_block;
    uint32_t blocks_per_group = sb->s_blocks_per_group;
    uint32_t inodes_per_group = sb->s_inodes_per_group;
    uint32_t num_groups = (sb->s_blocks_count + blocks_per_group - 1u) / blocks_per_group;

    uint32_t bgdt_blk = first_data_block + 1u;
    uint32_t bgdt_bytes = num_groups * (uint32_t) sizeof(ext2_bgd_t);
    uint32_t bgdt_blocks = (bgdt_bytes + block_size - 1u) / block_size;

    uint8_t *bgdt_buf = (uint8_t *) kmalloc(bgdt_blocks * block_size);
    if (!bgdt_buf) return false;

    for (uint32_t i = 0; i < bgdt_blocks; i++) {
        uint64_t lba = (uint64_t) (bgdt_blk + i) * (block_size / 512u);
        uint32_t secs = block_size / 512u;
        if (dev->ops->read(dev, lba, secs, bgdt_buf + i * block_size) < 0) {
            kfree(bgdt_buf);
            return false;
        }
    }

    ext2_bgd_t *bgdt = (ext2_bgd_t *) bgdt_buf;
    uint32_t group = (2u - 1u) / inodes_per_group;
    uint32_t idx = (2u - 1u) % inodes_per_group;
    uint64_t byteoff =
        (uint64_t) bgdt[group].bg_inode_table * block_size + (uint64_t) idx * inode_size;
    uint64_t lba = byteoff / 512u;
    uint32_t off = (uint32_t) (byteoff % 512u);

    uint8_t ino_buf[1024];
    if (dev->ops->read(dev, lba, 2u, ino_buf) < 0) {
        kfree(bgdt_buf);
        return false;
    }

    ext2_inode_t *root_ino = (ext2_inode_t *) (ino_buf + off);
    uint32_t nblocks = (uint32_t) ((root_ino->i_size + block_size - 1u) / block_size);
    uint32_t blk_secs = block_size / 512u;

    uint32_t bin_ino_nr = 0;
    for (uint32_t bi = 0; bi < nblocks && !bin_ino_nr; bi++) {
        uint32_t blk = root_ino->i_block[bi];
        if (!blk) continue;

        uint8_t *blk_buf = (uint8_t *) kmalloc(block_size);
        if (!blk_buf) break;

        uint64_t blk_lba = (uint64_t) blk * blk_secs;
        if (dev->ops->read(dev, blk_lba, blk_secs, blk_buf) < 0) {
            kfree(blk_buf);
            break;
        }

        uint32_t pos = 0;
        while (pos + 8u <= block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *) (blk_buf + pos);
            if (!de->rec_len || de->rec_len < 8u) break;
            if (de->inode && de->name_len) {
                bool dot = de->name_len == 1u && de->name[0] == '.';
                bool dotdot = de->name_len == 2u && de->name[0] == '.' && de->name[1] == '.';
                if (!dot && !dotdot && de->name_len == 3 && memcmp(de->name, "bin", 3) == 0) {
                    bin_ino_nr = de->inode;
                    break;
                }
            }
            pos += de->rec_len;
        }
        kfree(blk_buf);
    }

    if (!bin_ino_nr) {
        kfree(bgdt_buf);
        return false;
    }

    {
        uint32_t bg = (bin_ino_nr - 1u) / inodes_per_group;
        uint32_t ix = (bin_ino_nr - 1u) % inodes_per_group;
        uint64_t bo = (uint64_t) bgdt[bg].bg_inode_table * block_size + (uint64_t) ix * inode_size;
        uint64_t la = bo / 512u;
        uint32_t of = (uint32_t) (bo % 512u);
        if (dev->ops->read(dev, la, 2u, ino_buf) < 0) {
            kfree(bgdt_buf);
            return false;
        }
        ext2_inode_t *bin_ino = (ext2_inode_t *) (ino_buf + of);
        uint32_t bn = (uint32_t) ((bin_ino->i_size + block_size - 1u) / block_size);

        bool found = false;
        for (uint32_t b = 0; b < bn && !found; b++) {
            uint32_t bk = bin_ino->i_block[b];
            if (!bk) continue;

            uint8_t *bb = (uint8_t *) kmalloc(block_size);
            if (!bb) break;
            uint64_t bl = (uint64_t) bk * blk_secs;
            if (dev->ops->read(dev, bl, blk_secs, bb) < 0) {
                kfree(bb);
                break;
            }

            uint32_t p = 0;
            while (p + 8u <= block_size) {
                ext2_dirent_t *d = (ext2_dirent_t *) (bb + p);
                if (!d->rec_len || d->rec_len < 8u) break;
                if (d->inode && d->name_len) {
                    bool dt = d->name_len == 1u && d->name[0] == '.';
                    bool dd = d->name_len == 2u && d->name[0] == '.' && d->name[1] == '.';
                    if (!dt && !dd && d->name_len == 4 && memcmp(d->name, "init", 4) == 0) {
                        found = true;
                        break;
                    }
                }
                p += d->rec_len;
            }
            kfree(bb);
        }

        kfree(bgdt_buf);
        return found;
    }
}

bool ext2_mount(struct block_device *dev, const char *mount_point) {
    if (!dev || !dev->ops || !dev->ops->read || !mount_point) return false;
    if (g_dev) {
        log_error("ext2: only one mounted filesystem is currently supported");
        return false;
    }

    uint8_t sb_buf[1024];
    if (dev->ops->read(dev, EXT2_SB_LBA, 2u, sb_buf) < 0) {
        log_error("ext2: failed to read superblock");
        return false;
    }
    ext2_sb_t candidate;
    memcpy(&candidate, sb_buf, sizeof(candidate));

    if (candidate.s_magic != EXT2_MAGIC) {
        log_error("ext2: bad magic 0x%04x", candidate.s_magic);
        return false;
    }

    uint32_t unknown_incompat =
        candidate.s_feature_incompat & ~EXT2_INCOMPAT_FILETYPE;
    if (unknown_incompat) {
        log_error("ext2: unsupported incompat features 0x%x", unknown_incompat);
        return false;
    }
    uint32_t inode_size =
        candidate.s_rev_level >= 1u ? (uint32_t) candidate.s_inode_size : 128u;
    if (candidate.s_log_block_size > 2u ||
        !candidate.s_blocks_count ||
        !candidate.s_blocks_per_group ||
        !candidate.s_inodes_per_group) {
        log_error("ext2: invalid superblock geometry");
        return false;
    }
    uint32_t block_size = 1024u << candidate.s_log_block_size;
    if (inode_size < 128u || inode_size > block_size ||
        (inode_size & (inode_size - 1u)) || block_size % inode_size) {
        log_error("ext2: invalid inode geometry");
        return false;
    }

    g_dev = dev;
    g_tracked_cnt = 0;
    memcpy(&g_sb, &candidate, sizeof(g_sb));

    size_t mplen = strlen(mount_point);
    if (mplen >= sizeof(g_mount_point)) mplen = sizeof(g_mount_point) - 1u;
    memcpy(g_mount_point, mount_point, mplen);
    g_mount_point[mplen] = '\0';

    g_block_size = block_size;
    g_blocks_per_group = g_sb.s_blocks_per_group;
    g_inodes_per_group = g_sb.s_inodes_per_group;
    g_first_data_block = g_sb.s_first_data_block;
    g_inode_size = inode_size;
    g_num_groups = (g_sb.s_blocks_count + g_blocks_per_group - 1u) / g_blocks_per_group;
    g_has_large_file = !!(g_sb.s_feature_ro_compat & EXT2_RO_COMPAT_LARGE);

    g_meta_cache = (uint8_t *) kmalloc((uint64_t) EXT2_META_CACHE_ENTRIES * g_block_size);
    g_block_hints = (uint32_t *) kmalloc((uint64_t) g_num_groups * sizeof(uint32_t));
    g_inode_hints = (uint32_t *) kmalloc((uint64_t) g_num_groups * sizeof(uint32_t));
    if (!g_meta_cache || !g_block_hints || !g_inode_hints) {
        clear_mount_state();
        return false;
    }
    memset(g_meta_cache_valid, 0, sizeof(g_meta_cache_valid));
    memset(g_block_hints, 0, (uint64_t) g_num_groups * sizeof(uint32_t));
    memset(g_inode_hints, 0, (uint64_t) g_num_groups * sizeof(uint32_t));

    log_info("ext2: block_size=%u  groups=%u  inode_size=%u  large_file=%d", g_block_size,
             g_num_groups, g_inode_size, (int) g_has_large_file);

    g_indir_buf = (uint8_t *) kmalloc(g_block_size);
    if (!g_indir_buf) {
        clear_mount_state();
        return false;
    }

    uint32_t bgdt_blk = g_first_data_block + 1u;
    uint32_t bgdt_bytes = g_num_groups * (uint32_t) sizeof(ext2_bgd_t);
    uint32_t bgdt_blocks = (bgdt_bytes + g_block_size - 1u) / g_block_size;

    g_bgdt = (ext2_bgd_t *) kmalloc(bgdt_blocks * g_block_size);
    if (!g_bgdt) {
        clear_mount_state();
        return false;
    }

    for (uint32_t i = 0; i < bgdt_blocks; i++) {
        if (read_block(bgdt_blk + i, (uint8_t *) g_bgdt + i * g_block_size) < 0) {
            log_error("ext2: failed to read BGDT block %u", i);
            clear_mount_state();
            return false;
        }
    }

    vfs_mkdir_p(mount_point, 0755u);
    walk_dir(2u, mount_point);

    log_info("ext2: mounted at %s  (%d nodes)", mount_point, g_tracked_cnt);
    return true;
}

static int ext2_fs_create(vfs_node_t *n, const char *path, uint32_t mode) {
    (void) mode;
    if (!g_dev || !g_mount_point[0] || !path) return -1;

    size_t mplen = strlen(g_mount_point);
    if (!(mplen == 1u && g_mount_point[0] == '/') &&
        (strncmp(path, g_mount_point, mplen) != 0 ||
         (path[mplen] != '\0' && path[mplen] != '/')))
        return -1;

    uint32_t parent_ino;
    const char *childname;
    if (resolve_parent(path, &parent_ino, &childname) < 0 || !*childname) return -1;

    if (ext2_create(path, (uint16_t) (n->mode & 07777u), NULL, 0) < 0)
        return -1;

    uint32_t ino_nr = ext2_lookup(parent_ino, childname);
    if (!ino_nr) return -1;

    n->fs_ops = &ext2_reg_ops;
    n->fs_private = (void *) (uintptr_t) ino_nr;
    track_node(ino_nr, n);
    return 0;
}

static bool ext2_fs_unmount(const char *mount_point) {
    if (!g_dev || !mount_point || strcmp(mount_point, g_mount_point) != 0)
        return false;
    if (ext2_sync() < 0) return false;
    clear_mount_state();
    return true;
}

static struct filesystem ext2_fs = {
    .name = "ext2",
    .check_root = ext2_check_root,
    .mount = ext2_mount,
    .unmount = ext2_fs_unmount,
    .sync = ext2_sync,
    .create = ext2_fs_create,
};

void ext2_init(void) { vfs_register_fs(&ext2_fs); }
