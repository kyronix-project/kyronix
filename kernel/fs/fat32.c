#include "fat32.h"
#include "lib/log.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "vfs.h"

#define FAT32_EOC 0x0FFFFFF8u
#define FAT32_BAD 0x0FFFFFF7u
#define FAT32_MASK 0x0FFFFFFFu

#define FAT_ATTR_RO 0x01
#define FAT_ATTR_HID 0x02
#define FAT_ATTR_SYS 0x04
#define FAT_ATTR_VOL 0x08
#define FAT_ATTR_DIR 0x10
#define FAT_ATTR_ARC 0x20
#define FAT_ATTR_LFN 0x0F

/* 255 UTF-16 chars max, 13 per LFN entry */
#define FAT32_LFN_SLOTS 20
#define FAT32_MAX_FILE (64u * 1024u * 1024u)
#define FAT32_MAX_DEPTH 16

typedef struct __attribute__((packed)) {
    uint8_t jmp[3];
    char oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_sector;
    uint8_t _res[12];
    uint8_t drive_num;
    uint8_t _res2;
    uint8_t boot_sig;
    uint32_t vol_id;
    char vol_label[11];
    char fs_type[8];
} fat32_bpb_t;

typedef struct __attribute__((packed)) {
    char name[8];
    char ext[3];
    uint8_t attr;
    uint8_t _nt;
    uint8_t create_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_hi;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t cluster_lo;
    uint32_t size;
} fat_dirent_t;

typedef struct __attribute__((packed)) {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t _cluster;
    uint16_t name3[2];
} fat_lfn_t;

typedef struct {
    fat32_read_fn read;
    void *ctx;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t fat_size;
    uint32_t data_start;
    uint32_t root_cluster;
    uint32_t total_clusters;
    uint8_t *fat_sec;
    uint64_t fat_sec_lba;
} fat32_ctx_t;

static uint32_t fat_next(fat32_ctx_t *ctx, uint32_t cluster) {
    uint32_t fat_off = cluster * 4;
    uint64_t lba = ctx->reserved_sectors + fat_off / ctx->bytes_per_sector;
    uint32_t off = fat_off % ctx->bytes_per_sector;
    if (lba != ctx->fat_sec_lba) {
        if (ctx->read(ctx->ctx, lba, 1, ctx->fat_sec) < 0) return FAT32_EOC;
        ctx->fat_sec_lba = lba;
    }
    uint32_t v;
    memcpy(&v, ctx->fat_sec + off, 4);
    return v & FAT32_MASK;
}

static uint8_t *read_cluster(fat32_ctx_t *ctx, uint32_t cluster) {
    if (cluster < 2 || cluster > ctx->total_clusters + 1) return NULL;
    uint64_t lba = ctx->data_start + (uint64_t) (cluster - 2) * ctx->sectors_per_cluster;
    uint8_t *buf = kmalloc(ctx->cluster_size);
    if (!buf) return NULL;
    if (ctx->read(ctx->ctx, lba, ctx->sectors_per_cluster, buf) < 0) {
        kfree(buf);
        return NULL;
    }
    return buf;
}

static void lfn_place(const fat_lfn_t *lfn, uint16_t *buf) {
    uint32_t base = (uint32_t) ((lfn->order & 0x3F) - 1) * 13;
    for (int i = 0; i < 5; i++) buf[base + i] = lfn->name1[i];
    for (int i = 0; i < 6; i++) buf[base + 5 + i] = lfn->name2[i];
    for (int i = 0; i < 2; i++) buf[base + 11 + i] = lfn->name3[i];
}

static void ucs2_to_utf8(const uint16_t *src, int nchars, char *dst, int dstlen) {
    int di = 0;
    for (int i = 0; i < nchars && di < dstlen - 1; i++) {
        uint16_t c = src[i];
        if (c == 0x0000 || c == 0xFFFF) break;
        if (c < 0x0080) {
            dst[di++] = (char) c;
        } else if (c < 0x0800) {
            if (di + 2 >= dstlen) break;
            dst[di++] = (char) (0xC0 | (c >> 6));
            dst[di++] = (char) (0x80 | (c & 0x3F));
        } else {
            if (di + 3 >= dstlen) break;
            dst[di++] = (char) (0xE0 | (c >> 12));
            dst[di++] = (char) (0x80 | ((c >> 6) & 0x3F));
            dst[di++] = (char) (0x80 | (c & 0x3F));
        }
    }
    dst[di] = '\0';
}

static char fat_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char) (c + 32) : c; }

static bool fat_name_safe(const char *name) {
    if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (const unsigned char *p = (const unsigned char *) name; *p; p++) {
        if (*p < 0x20 || *p == '/' || *p == '\\' || *p == ':' || *p == '*' || *p == '?' ||
            *p == '"' || *p == '<' || *p == '>' || *p == '|')
            return false;
    }
    return true;
}

static void build_83_name(const fat_dirent_t *e, char *out) {
    bool lc_name = (e->_nt & 0x08) != 0;
    bool lc_ext = (e->_nt & 0x10) != 0;
    int n = 0;
    for (int i = 0; i < 8 && e->name[i] != ' '; i++)
        out[n++] = lc_name ? fat_lower(e->name[i]) : e->name[i];
    bool has_ext = false;
    for (int i = 0; i < 3; i++)
        if (e->ext[i] != ' ') {
            has_ext = true;
            break;
        }
    if (has_ext) {
        out[n++] = '.';
        for (int i = 0; i < 3 && e->ext[i] != ' '; i++)
            out[n++] = lc_ext ? fat_lower(e->ext[i]) : e->ext[i];
    }
    out[n] = '\0';
}

static uint8_t *read_chain(fat32_ctx_t *ctx, uint32_t start, uint32_t fsize) {
    if (fsize == 0 || fsize > FAT32_MAX_FILE) return NULL;
    uint32_t alloc_sz = (fsize + ctx->cluster_size - 1) / ctx->cluster_size * ctx->cluster_size;
    uint8_t *buf = kmalloc(alloc_sz);
    if (!buf) return NULL;
    uint64_t done = 0;
    uint32_t cl = start;
    while ((cl & FAT32_MASK) < FAT32_BAD && cl >= 2 && cl <= ctx->total_clusters + 1 &&
           done < fsize) {
        uint64_t lba = ctx->data_start + (uint64_t) (cl - 2) * ctx->sectors_per_cluster;
        if (ctx->read(ctx->ctx, lba, ctx->sectors_per_cluster, buf + done) < 0) break;
        done += ctx->cluster_size;
        cl = fat_next(ctx, cl);
    }
    if (done < fsize) memset(buf + done, 0, fsize - done);
    return buf;
}

static void walk_dir(fat32_ctx_t *ctx, uint32_t start_cl, const char *path, int depth, int *count);

static void process_dirent(fat32_ctx_t *ctx, const fat_dirent_t *e, const char *name,
                           const char *path, int depth, int *count) {
    char fullpath[512];
    size_t plen = strlen(path);
    size_t nlen = strlen(name);
    if (plen + 1 + nlen >= sizeof(fullpath)) return;
    memcpy(fullpath, path, plen);
    if (plen == 0 || path[plen - 1] != '/') fullpath[plen++] = '/';
    memcpy(fullpath + plen, name, nlen + 1);

    uint32_t cluster = ((uint32_t) e->cluster_hi << 16) | e->cluster_lo;

    if (e->attr & FAT_ATTR_DIR) {
        vfs_mkdir_p(fullpath, 0755);
        if (depth < FAT32_MAX_DEPTH) walk_dir(ctx, cluster, fullpath, depth + 1, count);
        return;
    }

    uint8_t *data = read_chain(ctx, cluster, e->size);
    if (e->size > 0 && !data) {
        log_warn("FAT32: skipping %s (size=%u)", fullpath, (unsigned) e->size);
        return;
    }
    uint32_t mode = (e->attr & FAT_ATTR_RO) ? 0444 : 0644;
    vfs_node_t *n = vfs_create_file(fullpath, mode, data, e->size);
    if (data) kfree(data);
    if (n) (*count)++;
}

static void walk_dir(fat32_ctx_t *ctx, uint32_t start_cl, const char *path, int depth, int *count) {
    uint16_t lfn_buf[FAT32_LFN_SLOTS * 13 + 1];
    memset(lfn_buf, 0xFF, sizeof(lfn_buf));
    bool have_lfn = false;

#define RESET_LFN()                                                                                \
    do {                                                                                           \
        have_lfn = false;                                                                          \
        memset(lfn_buf, 0xFF, sizeof(lfn_buf));                                                    \
    } while (0)

    uint32_t cl = start_cl;
    uint32_t traversed = 0;
    while ((cl & FAT32_MASK) < FAT32_BAD && cl >= 2 && cl <= ctx->total_clusters + 1 &&
           traversed++ < ctx->total_clusters) {
        uint8_t *cdata = read_cluster(ctx, cl);
        if (!cdata) break;

        uint32_t nents = ctx->cluster_size / sizeof(fat_dirent_t);
        const fat_dirent_t *dir = (const fat_dirent_t *) cdata;

        for (uint32_t i = 0; i < nents; i++) {
            const fat_dirent_t *e = &dir[i];

            if ((uint8_t) e->name[0] == 0x00) goto next_cluster;
            if ((uint8_t) e->name[0] == 0xE5) {
                RESET_LFN();
                continue;
            }

            /* АААААААААААААААААааааааааааАААААААААААААА бұл қашан бітеді өзі бл... */
            if (e->attr == FAT_ATTR_LFN) {
                const fat_lfn_t *lfn = (const fat_lfn_t *) e;
                if (lfn->order & 0x40) RESET_LFN();
                uint8_t seq = lfn->order & 0x3F;
                if (seq >= 1 && seq <= FAT32_LFN_SLOTS) {
                    lfn_place(lfn, lfn_buf);
                    have_lfn = true;
                }
                continue;
            }
            if (e->attr & FAT_ATTR_VOL) {
                RESET_LFN();
                continue;
            }
            if (e->name[0] == '.' && (e->name[1] == ' ' || e->name[1] == '.')) {
                RESET_LFN();
                continue;
            }

            char fname[256];
            if (have_lfn) {
                ucs2_to_utf8(lfn_buf, FAT32_LFN_SLOTS * 13, fname, sizeof(fname));
                RESET_LFN();
            } else {
                build_83_name(e, fname);
            }
            if (!fat_name_safe(fname)) continue;

            process_dirent(ctx, e, fname, path, depth, count);
        }

    next_cluster:;
        uint32_t next_cl = fat_next(ctx, cl);
        kfree(cdata);
        cl = next_cl;
    }

#undef RESET_LFN
}

int fat32_mount(fat32_read_fn read, void *opaque, const char *mountpoint) {
    uint8_t *sector0 = kmalloc(4096);
    if (!sector0) return -1;
    if (read(opaque, 0, 1, sector0) < 0) {
        kfree(sector0);
        log_error("FAT32: failed to read boot sector");
        return -1;
    }
    if (sector0[510] != 0x55 || sector0[511] != 0xAA) {
        log_error("FAT32: bad boot sector signature");
        kfree(sector0);
        return -1;
    }

    fat32_bpb_t bpb;
    memcpy(&bpb, sector0, sizeof(bpb));
    kfree(sector0);

    if (bpb.bytes_per_sector < 512 || bpb.bytes_per_sector > 4096 ||
        (bpb.bytes_per_sector & (bpb.bytes_per_sector - 1)) != 0) {
        log_error("FAT32: invalid bytes_per_sector=%u", bpb.bytes_per_sector);
        return -1;
    }
    if (bpb.sectors_per_cluster == 0 || bpb.sectors_per_cluster > 128 ||
        (bpb.sectors_per_cluster & (bpb.sectors_per_cluster - 1)) != 0) {
        log_error("FAT32: invalid sectors_per_cluster=%u", bpb.sectors_per_cluster);
        return -1;
    }
    if (bpb.fat_size_32 == 0 || bpb.num_fats == 0 || bpb.reserved_sectors == 0) {
        log_error("FAT32: invalid FAT geometry");
        return -1;
    }

    uint64_t data_start =
        (uint64_t) bpb.reserved_sectors + (uint64_t) bpb.num_fats * bpb.fat_size_32;
    if (data_start > UINT32_MAX) return -1;
    fat32_ctx_t ctx = {
        .read = read,
        .ctx = opaque,
        .bytes_per_sector = bpb.bytes_per_sector,
        .sectors_per_cluster = bpb.sectors_per_cluster,
        .cluster_size = bpb.bytes_per_sector * bpb.sectors_per_cluster,
        .reserved_sectors = bpb.reserved_sectors,
        .num_fats = bpb.num_fats,
        .fat_size = bpb.fat_size_32,
        .data_start = (uint32_t) data_start,
        .root_cluster = bpb.root_cluster,
        .fat_sec_lba = UINT64_MAX,
    };

    uint32_t total_sectors = bpb.total_sectors_32 ? bpb.total_sectors_32 : bpb.total_sectors_16;
    uint32_t data_sectors = total_sectors > ctx.data_start ? total_sectors - ctx.data_start : 0;
    ctx.total_clusters = data_sectors / ctx.sectors_per_cluster;
    uint64_t fat_bytes = (uint64_t) ctx.fat_size * ctx.bytes_per_sector;
    if (ctx.total_clusters == 0 || ctx.total_clusters > FAT32_BAD - 2 ||
        ctx.root_cluster < 2 || ctx.root_cluster > ctx.total_clusters + 1 ||
        ((uint64_t) ctx.total_clusters + 2) * 4 > fat_bytes)
        return -1;

    ctx.fat_sec = kmalloc(ctx.bytes_per_sector);
    if (!ctx.fat_sec) return -1;

    log_info("FAT32: %u B/sec, %u sec/cluster, data LBA %u, root cluster %u", ctx.bytes_per_sector,
             ctx.sectors_per_cluster, ctx.data_start, ctx.root_cluster);

    char mp_buf[512];
    const char *mp = mountpoint;
    size_t mp_len = strlen(mountpoint);
    if (mp_len >= sizeof(mp_buf)) {
        kfree(ctx.fat_sec);
        return -1;
    }
    if (mp_len > 1) {
        memcpy(mp_buf, mountpoint, mp_len + 1);
        if (mp_buf[mp_len - 1] == '/') mp_buf[--mp_len] = '\0';
        mp = mp_buf;
    } else {
        mp = "";
    }

    int count = 0;
    walk_dir(&ctx, ctx.root_cluster, mp, 0, &count);
    kfree(ctx.fat_sec);
    log_info("FAT32: loaded %d files", count);
    return 0;
}

typedef struct {
    const uint8_t *base;
    uint64_t size;
    uint32_t sector_size;
} mem_disk_t;

static int mem_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    mem_disk_t *d = ctx;
    if (lba > UINT64_MAX / d->sector_size) return -1;
    uint64_t off = lba * d->sector_size;
    uint64_t len = (uint64_t) count * d->sector_size;
    if (off > d->size || len > d->size - off) return -1;
    memcpy(buf, d->base + off, len);
    return 0;
}

int fat32_mount_mem(const void *image, uint64_t size, const char *mountpoint) {
    if (!image || size < 512) return -1;
    mem_disk_t d = { .base = image, .size = size, .sector_size = 512 };
    uint16_t bps;
    memcpy(&bps, (const uint8_t *) image + 11, 2);
    if (bps >= 512 && bps <= 4096 && (bps & (bps - 1)) == 0) d.sector_size = bps;
    return fat32_mount(mem_read, &d, mountpoint);
}
