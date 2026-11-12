#include "squashfs.h"
#include "../drivers/block/block.h"
#include "../lib/inflate.h"
#include "../lib/log.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../lib/xz/xz_kernel.h"
#include "../lib/zstd/zstd_kernel.h"
#include "../mm/heap.h"
#include "vfs.h"
#include "vfs_internal.h"

#define SQFS_MAGIC 0x73717368u
#define SQFS_MAJOR 4
#define SQFS_MINOR 0
#define SQFS_COMP_ZLIB 1
#define SQFS_COMP_XZ 4
#define SQFS_COMP_ZSTD 6
#define SQFS_META_SIZE 8192
#define SQFS_NAME_MAX 256
#define SQFS_DIR_COUNT_MAX 256
#define SQFS_MAX_BLOCK_LOG 20
#define SQFS_MIN_BLOCK_LOG 12
#define SQFS_INVAL_FRAG 0xffffffffu
#define SQFS_UNCOMP_BIT_META (1u << 15)
#define SQFS_UNCOMP_BIT_BLOCK (1u << 24)
#define SQFS_META_CACHE_SLOTS 64
#define SQFS_MAX_MOUNTS 8
#define SQFS_MAX_DEPTH 48
#define SQFS_MAX_NODES 200000
#define SQFS_READ_CHUNK_SECTORS 64

#define SQFS_T_DIR 1
#define SQFS_T_REG 2
#define SQFS_T_SYMLINK 3
#define SQFS_T_BLKDEV 4
#define SQFS_T_CHRDEV 5
#define SQFS_T_FIFO 6
#define SQFS_T_SOCK 7
#define SQFS_T_LDIR 8
#define SQFS_T_LREG 9
#define SQFS_T_LSYMLINK 10
#define SQFS_T_LBLKDEV 11
#define SQFS_T_LCHRDEV 12
#define SQFS_T_LFIFO 13
#define SQFS_T_LSOCK 14

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint32_t inodes;
	uint32_t mkfs_time;
	uint32_t block_size;
	uint32_t fragments;
	uint16_t compression;
	uint16_t block_log;
	uint16_t flags;
	uint16_t no_ids;
	uint16_t s_major;
	uint16_t s_minor;
	uint64_t root_inode;
	uint64_t bytes_used;
	uint64_t id_table_start;
	uint64_t xattr_id_table_start;
	uint64_t inode_table_start;
	uint64_t directory_table_start;
	uint64_t fragment_table_start;
	uint64_t lookup_table_start;
} sqfs_sb_t;

typedef struct __attribute__((packed)) {
	uint32_t count;
	uint32_t start_block;
	uint32_t inode_number;
} sqfs_dir_hdr_t;

typedef struct __attribute__((packed)) {
	uint16_t offset;
	uint16_t inode_number;
	uint16_t type;
	uint16_t size;
} sqfs_dirent_t;

typedef struct __attribute__((packed)) {
	uint64_t start_block;
	uint32_t size;
	uint32_t unused;
} sqfs_frag_entry_t;

typedef struct {
	uint64_t pos;
	uint32_t len;
	uint32_t span;
	uint8_t *data;
} sqfs_meta_slot_t;

typedef struct {
	struct block_device *dev;
	uint16_t compression;
	uint32_t block_size;
	uint32_t block_log;
	uint64_t bytes_used;
	uint64_t inode_table;
	uint64_t dir_table;
	uint32_t fragments;
	uint32_t ids;
	uint64_t *frag_index;
	uint32_t frag_index_len;
	uint64_t *id_index;
	uint32_t id_index_len;
	uint64_t root_ref;
	uint32_t node_count;
	vfs_node_t *root_node;
	char mount_point[512];
	sqfs_meta_slot_t meta[SQFS_META_CACHE_SLOTS];
	uint32_t meta_next;
} sqfs_t;

typedef struct {
	sqfs_t *sq;
	uint64_t start_block;
	uint64_t file_size;
	uint32_t fragment;
	uint32_t frag_offset;
	uint32_t nblocks;
	uint32_t block_list[];
} sqfs_file_t;

typedef struct {
	sqfs_t *sq;
	uint64_t block;
	uint32_t off;
} sqfs_stream_t;

typedef struct {
	uint16_t type;
	uint16_t mode;
	uint32_t uid_idx;
	uint32_t gid_idx;
	uint32_t mtime;
	uint32_t ino_number;
	uint64_t start_block;
	uint64_t file_size;
	uint32_t fragment;
	uint32_t frag_offset;
	uint64_t sparse;
	uint32_t nlink;
	uint32_t symlink_size;
	char *symlink;
	uint32_t nblocks;
	uint32_t *block_list;
	uint32_t rdev;
	uint32_t dir_off;
} sqfs_ino_t;

static sqfs_t *g_sqfs_mounts[SQFS_MAX_MOUNTS];

static int sqfs_read_bytes(sqfs_t *sq, uint64_t abs, void *dst, uint64_t n) {
	if (!sq || !sq->dev || !sq->dev->ops || !sq->dev->ops->read) return -1;
	if (!dst || !n) return -1;
	if (abs > sq->bytes_used || n > sq->bytes_used - abs) return -1;

	uint32_t ss = sq->dev->sector_size ? sq->dev->sector_size : 512u;
	uint8_t *chunk = NULL;
	uint32_t chunk_sectors = 0;
	uint64_t done = 0;

	while (done < n) {
		uint64_t cur = abs + done;
		uint64_t lba = cur / ss;
		uint64_t end = cur + (n - done);
		uint64_t end_lba = (end + ss - 1) / ss;
		uint64_t count = end_lba - lba;
		if (count > SQFS_READ_CHUNK_SECTORS) count = SQFS_READ_CHUNK_SECTORS;
		uint64_t bytes = count * ss;

		if (!chunk || chunk_sectors < count) {
			if (chunk) kfree(chunk);
			chunk = (uint8_t *) kmalloc((uint64_t) SQFS_READ_CHUNK_SECTORS * ss);
			if (!chunk) return -1;
			chunk_sectors = SQFS_READ_CHUNK_SECTORS;
		}

		if (sq->dev->ops->read(sq->dev, lba, (uint32_t) count, chunk) < 0) {
			if (chunk) kfree(chunk);
			return -1;
		}

		uint64_t in_off = cur - lba * ss;
		uint64_t avail = bytes - in_off;
		uint64_t take = n - done;
		if (take > avail) take = avail;
		memcpy((uint8_t *) dst + done, chunk + in_off, take);
		done += take;
	}

	if (chunk) kfree(chunk);
	return 0;
}

static int64_t sqfs_zlib_decompress(const uint8_t *src, uint64_t src_len, uint8_t *dst,
                                    uint64_t dst_cap) {
	if (src_len <= 2) return -1;
	if ((src[0] & 0x0fu) != 8u) return -1;
	if ((src[0] >> 4) > 7u) return -1;
	if (((uint32_t) src[0] * 256u + (uint32_t) src[1]) % 31u != 0u) return -1;
	if (src[1] & 0x20u) return -1;
	return kinflate(src + 2, src_len - 2, dst, dst_cap);
}

static int64_t sqfs_decompress(sqfs_t *sq, const uint8_t *src, uint64_t src_len, uint8_t *dst,
                               uint64_t dst_cap) {
	if (sq->compression == SQFS_COMP_XZ) return kxz_decode(src, src_len, dst, dst_cap);
	if (sq->compression == SQFS_COMP_ZSTD) return kzstd_decode(src, src_len, dst, dst_cap);
	return sqfs_zlib_decompress(src, src_len, dst, dst_cap);
}

static sqfs_meta_slot_t *sqfs_meta_get(sqfs_t *sq, uint64_t pos) {
	if (!sq || pos + 2 > sq->bytes_used) return NULL;

	for (int i = 0; i < SQFS_META_CACHE_SLOTS; i++) {
		sqfs_meta_slot_t *s = &sq->meta[i];
		if (s->data && s->pos == pos && s->len) return s;
	}

	uint8_t hdr[2];
	if (sqfs_read_bytes(sq, pos, hdr, 2) < 0) return NULL;
	uint16_t v = (uint16_t) hdr[0] | ((uint16_t) hdr[1] << 8);
	uint32_t uncompressed = (v & SQFS_UNCOMP_BIT_META) != 0;
	uint32_t on_disk = v & (SQFS_UNCOMP_BIT_META - 1);
	if (!on_disk) return NULL;
	if (pos + 2 + on_disk > sq->bytes_used) return NULL;

	sqfs_meta_slot_t *slot = NULL;
	for (int i = 0; i < SQFS_META_CACHE_SLOTS; i++) {
		if (!sq->meta[i].data) {
			slot = &sq->meta[i];
			break;
		}
	}
	if (!slot) {
		slot = &sq->meta[sq->meta_next];
		sq->meta_next = (sq->meta_next + 1) % SQFS_META_CACHE_SLOTS;
	}
	if (!slot->data) {
		slot->data = (uint8_t *) kmalloc(SQFS_META_SIZE);
		if (!slot->data) return NULL;
	}

	slot->pos = pos;
	slot->len = 0;
	slot->span = 2 + on_disk;

	if (uncompressed) {
		if (on_disk > SQFS_META_SIZE) return NULL;
		if (sqfs_read_bytes(sq, pos + 2, slot->data, on_disk) < 0) return NULL;
		slot->len = on_disk;
	}
	else {
		uint8_t *src = (uint8_t *) kmalloc(on_disk);
		if (!src) return NULL;
		if (sqfs_read_bytes(sq, pos + 2, src, on_disk) < 0) {
			kfree(src);
			return NULL;
		}
		int64_t n = sqfs_decompress(sq, src, on_disk, slot->data, SQFS_META_SIZE);
		kfree(src);
		if (n <= 0 || (uint64_t) n > SQFS_META_SIZE) return NULL;
		slot->len = (uint32_t) n;
	}

	return slot;
}

static int sqfs_stream_read(sqfs_stream_t *ms, void *dst, uint64_t n) {
	if (!ms || !dst) return -1;
	uint64_t done = 0;

	while (done < n) {
		sqfs_meta_slot_t *slot = sqfs_meta_get(ms->sq, ms->block);
		if (!slot) return -1;
		if (ms->off >= slot->len) return -1;
		uint64_t avail = slot->len - ms->off;
		uint64_t take = n - done;
		if (take > avail) take = avail;
		memcpy((uint8_t *) dst + done, slot->data + ms->off, take);
		done += take;
		ms->off += (uint32_t) take;
		if (ms->off == slot->len) {
			ms->block += slot->span;
			ms->off = 0;
		}
	}

	return 0;
}

static int sqfs_rd16(sqfs_stream_t *ms, uint16_t *out) {
	uint8_t b[2];
	if (sqfs_stream_read(ms, b, 2) < 0) return -1;
	*out = (uint16_t) b[0] | ((uint16_t) b[1] << 8);
	return 0;
}

static int sqfs_rd32(sqfs_stream_t *ms, uint32_t *out) {
	uint8_t b[4];
	if (sqfs_stream_read(ms, b, 4) < 0) return -1;
	*out = (uint32_t) b[0] | ((uint32_t) b[1] << 8) | ((uint32_t) b[2] << 16) |
	       ((uint32_t) b[3] << 24);
	return 0;
}

static int sqfs_rd64(sqfs_stream_t *ms, uint64_t *out) {
	uint32_t lo, hi;
	if (sqfs_rd32(ms, &lo) < 0) return -1;
	if (sqfs_rd32(ms, &hi) < 0) return -1;
	*out = (uint64_t) lo | ((uint64_t) hi << 32);
	return 0;
}

static uint32_t sqfs_id(sqfs_t *sq, uint32_t idx) {
	if (!sq->id_index || !sq->id_index_len || idx >= sq->ids) return 0;
	uint32_t per_block = SQFS_META_SIZE / 4;
	uint32_t bi = idx / per_block;
	uint32_t off = (idx % per_block) * 4;
	if (bi >= sq->id_index_len) return 0;

	sqfs_stream_t ms = { sq, sq->id_index[bi], off };
	uint32_t val = 0;
	if (sqfs_rd32(&ms, &val) < 0) return 0;
	return val;
}

static int sqfs_frag_lookup(sqfs_t *sq, uint32_t idx, uint64_t *start, uint32_t *raw_size) {
	if (idx >= sq->fragments) return -1;
	if (!sq->frag_index || !sq->frag_index_len) return -1;

	uint32_t per_block = SQFS_META_SIZE / sizeof(sqfs_frag_entry_t);
	uint32_t bi = idx / per_block;
	uint32_t off = (idx % per_block) * sizeof(sqfs_frag_entry_t);
	if (bi >= sq->frag_index_len) return -1;

	sqfs_stream_t ms = { sq, sq->frag_index[bi], off };
	sqfs_frag_entry_t e;
	if (sqfs_rd64(&ms, &e.start_block) < 0) return -1;
	if (sqfs_rd32(&ms, &e.size) < 0) return -1;
	if (sqfs_rd32(&ms, &e.unused) < 0) return -1;
	if (e.start_block + 2 > sq->bytes_used) return -1;
	if (e.size >> 25) return -1;

	*start = e.start_block;
	*raw_size = e.size;
	return 0;
}

static int sqfs_load_inode(sqfs_t *sq, uint64_t ref, sqfs_ino_t *ino) {
	if (!ino) return -1;
	memset(ino, 0, sizeof(*ino));
	if ((ref & 0xffffu) >= SQFS_META_SIZE) return -1;

	sqfs_stream_t ms = { sq, sq->inode_table + (ref >> 16), (uint32_t) (ref & 0xffffu) };
	uint16_t ino_type = 0;
	uint16_t mode = 0;
	uint16_t uid = 0;
	uint16_t guid = 0;
	uint32_t mtime = 0;
	uint32_t ino_number = 0;

	if (sqfs_rd16(&ms, &ino_type) < 0) return -1;
	if (sqfs_rd16(&ms, &mode) < 0) return -1;
	if (sqfs_rd16(&ms, &uid) < 0) return -1;
	if (sqfs_rd16(&ms, &guid) < 0) return -1;
	if (sqfs_rd32(&ms, &mtime) < 0) return -1;
	if (sqfs_rd32(&ms, &ino_number) < 0) return -1;

	ino->type = ino_type;
	ino->mode = mode;
	ino->uid_idx = uid;
	ino->gid_idx = guid;
	ino->mtime = mtime;
	ino->ino_number = ino_number;
	ino->fragment = SQFS_INVAL_FRAG;

	if (ino_type == SQFS_T_REG) {
		uint32_t start = 0, frag = 0, frag_off = 0, fsize = 0;
		if (sqfs_rd32(&ms, &start) < 0) return -1;
		if (sqfs_rd32(&ms, &frag) < 0) return -1;
		if (sqfs_rd32(&ms, &frag_off) < 0) return -1;
		if (sqfs_rd32(&ms, &fsize) < 0) return -1;
		if (frag != SQFS_INVAL_FRAG && (fsize & (sq->block_size - 1u)) == 0) return -1;
		ino->start_block = start;
		ino->file_size = fsize;
		ino->fragment = frag;
		ino->frag_offset = frag_off;
		ino->nblocks = frag != SQFS_INVAL_FRAG ? fsize >> sq->block_log
		                                       : (fsize + sq->block_size - 1u) >> sq->block_log;
	}
	else if (ino_type == SQFS_T_LREG) {
		uint32_t nlink = 0, frag = 0, frag_off = 0, xattr = 0;
		if (sqfs_rd64(&ms, &ino->start_block) < 0) return -1;
		if (sqfs_rd64(&ms, &ino->file_size) < 0) return -1;
		if (sqfs_rd64(&ms, &ino->sparse) < 0) return -1;
		if (sqfs_rd32(&ms, &nlink) < 0) return -1;
		if (sqfs_rd32(&ms, &frag) < 0) return -1;
		if (sqfs_rd32(&ms, &frag_off) < 0) return -1;
		if (sqfs_rd32(&ms, &xattr) < 0) return -1;
		if (frag != SQFS_INVAL_FRAG && (ino->file_size & (sq->block_size - 1u)) == 0) return -1;
		ino->nlink = nlink;
		ino->fragment = frag;
		ino->frag_offset = frag_off;
		uint64_t total = ino->file_size;
		if (ino->sparse > total) return -1;
		ino->nblocks = frag != SQFS_INVAL_FRAG ? (uint32_t) (total >> sq->block_log)
		                                       : (uint32_t) ((total + sq->block_size - 1u) >>
		                                                     sq->block_log);
		if (ino->nblocks > (1u << 20)) return -1;
	}
	else if (ino_type == SQFS_T_SYMLINK || ino_type == SQFS_T_LSYMLINK) {
		uint32_t nlink = 0, size = 0;
		if (sqfs_rd32(&ms, &nlink) < 0) return -1;
		if (sqfs_rd32(&ms, &size) < 0) return -1;
		if (size == 0 || size >= 512) return -1;
		ino->nlink = nlink;
		ino->symlink_size = size;
		ino->symlink = (char *) kmalloc(size + 1);
		if (!ino->symlink) return -1;
		if (sqfs_stream_read(&ms, ino->symlink, size) < 0) {
			kfree(ino->symlink);
			ino->symlink = NULL;
			return -1;
		}
		ino->symlink[size] = '\0';
		ino->file_size = size;
		if (ino_type == SQFS_T_LSYMLINK) {
			uint32_t xattr = 0;
			if (sqfs_rd32(&ms, &xattr) < 0) return -1;
		}
	}
	else if (ino_type == SQFS_T_DIR) {
		uint32_t start = 0, nlink = 0, parent = 0;
		uint16_t fsize = 0, off = 0;
		if (sqfs_rd32(&ms, &start) < 0) return -1;
		if (sqfs_rd32(&ms, &nlink) < 0) return -1;
		if (sqfs_rd16(&ms, &fsize) < 0) return -1;
		if (sqfs_rd16(&ms, &off) < 0) return -1;
		if (sqfs_rd32(&ms, &parent) < 0) return -1;
		ino->start_block = start;
		ino->file_size = fsize;
		ino->nlink = nlink;
		ino->dir_off = off;
	}
	else if (ino_type == SQFS_T_LDIR) {
		uint32_t nlink = 0, fsize = 0, start = 0, parent = 0, xattr = 0;
		uint16_t icount = 0, off = 0;
		if (sqfs_rd32(&ms, &nlink) < 0) return -1;
		if (sqfs_rd32(&ms, &fsize) < 0) return -1;
		if (sqfs_rd32(&ms, &start) < 0) return -1;
		if (sqfs_rd32(&ms, &parent) < 0) return -1;
		if (sqfs_rd16(&ms, &icount) < 0) return -1;
		if (sqfs_rd16(&ms, &off) < 0) return -1;
		if (sqfs_rd32(&ms, &xattr) < 0) return -1;
		ino->start_block = start;
		ino->file_size = fsize;
		ino->nlink = nlink;
		ino->dir_off = off;
	}
	else if (ino_type == SQFS_T_BLKDEV || ino_type == SQFS_T_CHRDEV ||
	         ino_type == SQFS_T_FIFO || ino_type == SQFS_T_SOCK) {
		uint32_t nlink = 0, rdev = 0;
		if (sqfs_rd32(&ms, &nlink) < 0) return -1;
		if (sqfs_rd32(&ms, &rdev) < 0) return -1;
		ino->nlink = nlink;
		ino->rdev = rdev;
	}
	else if (ino_type == SQFS_T_LBLKDEV || ino_type == SQFS_T_LCHRDEV ||
	         ino_type == SQFS_T_LFIFO || ino_type == SQFS_T_LSOCK) {
		uint32_t nlink = 0, rdev = 0, xattr = 0;
		if (sqfs_rd32(&ms, &nlink) < 0) return -1;
		if (sqfs_rd32(&ms, &rdev) < 0) return -1;
		if (sqfs_rd32(&ms, &xattr) < 0) return -1;
		ino->nlink = nlink;
		ino->rdev = rdev;
	}
	else {
		return -1;
	}

	if (ino->nblocks) {
		ino->block_list = (uint32_t *) kmalloc((uint64_t) ino->nblocks * 4);
		if (!ino->block_list) return -1;
		for (uint32_t i = 0; i < ino->nblocks; i++) {
			if (sqfs_rd32(&ms, &ino->block_list[i]) < 0) {
				kfree(ino->block_list);
				ino->block_list = NULL;
				return -1;
			}
			if (ino->block_list[i] >> 25) {
				kfree(ino->block_list);
				ino->block_list = NULL;
				return -1;
			}
		}
	}

	return 0;
}

static void sqfs_ino_free(sqfs_ino_t *ino) {
	if (ino->block_list) {
		kfree(ino->block_list);
		ino->block_list = NULL;
	}
	if (ino->symlink) {
		kfree(ino->symlink);
		ino->symlink = NULL;
	}
}

static int sqfs_read_block(sqfs_t *sq, uint64_t pos, uint32_t raw, uint8_t *dst, uint64_t expect) {
	if (!expect) return 0;
	if (raw == 0) {
		memset(dst, 0, expect);
		return 0;
	}
	uint32_t on_disk = raw & 0x00ffffffu;
	if (raw & SQFS_UNCOMP_BIT_BLOCK) {
		if (on_disk != expect) return -1;
		return sqfs_read_bytes(sq, pos, dst, on_disk);
	}
	if (!on_disk || pos + on_disk > sq->bytes_used) return -1;
	uint8_t *src = (uint8_t *) kmalloc(on_disk);
	if (!src) return -1;
	if (sqfs_read_bytes(sq, pos, src, on_disk) < 0) {
		kfree(src);
		return -1;
	}
	int64_t n = sqfs_decompress(sq, src, on_disk, dst, expect);
	kfree(src);
	if (n < 0 || (uint64_t) n != expect) return -1;
	return 0;
}

static int sqfs_read_fragment(sqfs_t *sq, uint32_t idx, uint32_t off, uint8_t *dst, uint64_t n) {
	uint64_t start = 0;
	uint32_t raw = 0;
	if (sqfs_frag_lookup(sq, idx, &start, &raw) < 0) return -1;

	uint8_t *frag = (uint8_t *) kmalloc(sq->block_size);
	if (!frag) return -1;

	uint32_t on_disk = raw & 0x00ffffffu;
	if (raw & SQFS_UNCOMP_BIT_BLOCK) {
		if (!on_disk || on_disk > sq->block_size || off + n > on_disk) {
			kfree(frag);
			return -1;
		}
		if (sqfs_read_bytes(sq, start, frag, on_disk) < 0) {
			kfree(frag);
			return -1;
		}
	}
	else {
		if (!on_disk || start + on_disk > sq->bytes_used) {
			kfree(frag);
			return -1;
		}
		uint8_t *src = (uint8_t *) kmalloc(on_disk);
		if (!src) {
			kfree(frag);
			return -1;
		}
		if (sqfs_read_bytes(sq, start, src, on_disk) < 0) {
			kfree(src);
			kfree(frag);
			return -1;
		}
		int64_t produced = sqfs_decompress(sq, src, on_disk, frag, sq->block_size);
		kfree(src);
		if (produced < 0 || (uint64_t) produced > sq->block_size) {
			kfree(frag);
			return -1;
		}
		if (off + n > (uint64_t) produced) {
			kfree(frag);
			return -1;
		}
	}

	memcpy(dst, frag + off, n);
	kfree(frag);
	return 0;
}

static int sqfs_populate_dir(sqfs_t *sq, vfs_node_t *dir_node, sqfs_ino_t *dir_ino, int depth);

static int sqfs_read_file_range(sqfs_file_t *f, uint8_t *dst, uint64_t off, uint64_t n);

static struct vfs_fs_ops sqfs_reg_ops;

static int sqfs_add_child(sqfs_t *sq, vfs_node_t *dir_node, const char *name, uint64_t child_ref,
                          int depth) {
	if (sq->node_count >= SQFS_MAX_NODES) return -1;

	sqfs_ino_t ci;
	if (sqfs_load_inode(sq, child_ref, &ci) < 0) return -1;

	vfs_node_t *n = NULL;
	uint16_t t = ci.type;

	if (t == SQFS_T_REG || t == SQFS_T_LREG) {
		n = vfs_node_alloc_internal(name, VFS_TYPE_REG, (ci.mode & 07777u) | S_IFREG);
		if (n) {
			sqfs_file_t *f = (sqfs_file_t *) kmalloc(sizeof(sqfs_file_t) +
			                                         (uint64_t) ci.nblocks * 4);
			if (!f) {
				sqfs_ino_free(&ci);
				kfree(n);
				return -1;
			}
			f->sq = sq;
			f->start_block = ci.start_block;
			f->file_size = ci.file_size;
			f->fragment = ci.fragment;
			f->frag_offset = ci.frag_offset;
			f->nblocks = ci.nblocks;
			if (ci.nblocks && ci.block_list) memcpy(f->block_list, ci.block_list,
			                                        (uint64_t) ci.nblocks * 4);
			n->size = ci.file_size;
			n->fs_ops = &sqfs_reg_ops;
			n->fs_private = f;
		}
	}
	else if (t == SQFS_T_SYMLINK || t == SQFS_T_LSYMLINK) {
		n = vfs_node_alloc_internal(name, VFS_TYPE_SYM, 0777u | S_IFLNK);
		if (n && ci.symlink) {
			n->symlink = ci.symlink;
			ci.symlink = NULL;
			n->size = ci.symlink_size;
		}
	}
	else if (t == SQFS_T_DIR || t == SQFS_T_LDIR) {
		n = vfs_node_alloc_internal(name, VFS_TYPE_DIR, (ci.mode & 07777u) | S_IFDIR);
	}
	else {
		sqfs_ino_free(&ci);
		return 0;
	}

	sqfs_ino_free(&ci);
	if (!n) return -1;

	n->ino = ci.ino_number;
	n->uid = sqfs_id(sq, ci.uid_idx);
	n->gid = sqfs_id(sq, ci.gid_idx);
	sq->node_count++;

	if (vfs_dir_find_internal(dir_node, name)) {
		if (n->fs_private) kfree(n->fs_private);
		if (n->symlink) kfree(n->symlink);
		kfree(n);
		return 0;
	}
	vfs_dir_insert_internal(dir_node, n);

	if (t == SQFS_T_DIR || t == SQFS_T_LDIR) {
		if (depth + 1 > SQFS_MAX_DEPTH) return 0;
		if (sqfs_populate_dir(sq, n, &ci, depth + 1) < 0) return -1;
	}

	return 0;
}

static int sqfs_populate_dir(sqfs_t *sq, vfs_node_t *dir_node, sqfs_ino_t *dir_ino, int depth) {
	if (!dir_node || dir_node->type != VFS_TYPE_DIR) return -1;
	if (dir_ino->file_size > (1u << 22)) return -1;

	sqfs_stream_t ms = { sq, sq->dir_table + dir_ino->start_block, dir_ino->dir_off };
	uint64_t consumed = 0;
	uint64_t total = dir_ino->file_size > 3 ? dir_ino->file_size - 3 : 0;
	int entries = 0;

	while (consumed < total) {
		sqfs_dir_hdr_t hdr;
		if (sqfs_rd32(&ms, &hdr.count) < 0) return 0;
		if (sqfs_rd32(&ms, &hdr.start_block) < 0) return 0;
		if (sqfs_rd32(&ms, &hdr.inode_number) < 0) return 0;
		consumed += 12;
		if (consumed > total) return 0;

		uint32_t count = hdr.count + 1;
		if (count > SQFS_DIR_COUNT_MAX) return 0;

		for (uint32_t i = 0; i < count; i++) {
			sqfs_dirent_t de;
			if (sqfs_rd16(&ms, &de.offset) < 0) return 0;
			if (sqfs_rd16(&ms, &de.inode_number) < 0) return 0;
			if (sqfs_rd16(&ms, &de.type) < 0) return 0;
			if (sqfs_rd16(&ms, &de.size) < 0) return 0;
			consumed += 8;

			uint32_t nlen = (uint32_t) de.size + 1;
			if (nlen > SQFS_NAME_MAX) return 0;
			char name[SQFS_NAME_MAX + 1];
			if (sqfs_stream_read(&ms, name, nlen) < 0) return 0;
			consumed += nlen;
			if (consumed > total) return 0;
			name[nlen] = '\0';

			if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
				continue;
			}

			entries++;
			if (entries > 100000) return 0;

			uint64_t child_ref = ((uint64_t) hdr.start_block << 16) | de.offset;
			if (sqfs_add_child(sq, dir_node, name, child_ref, depth) < 0) {
				log_warn("squashfs: skipping bad entry %s/%s", dir_node->name, name);
			}
		}
	}

	return 0;
}

static int64_t sqfs_reg_read(vfs_node_t *n, char *buf, uint64_t off, uint64_t len) {
	sqfs_file_t *f = (sqfs_file_t *) n->fs_private;
	if (!f || !f->sq) return -(int64_t) 5;
	if (off >= f->file_size) return 0;
	uint64_t avail = f->file_size - off;
	uint64_t r = len < avail ? len : avail;

	if (!buf) {
		if (!r) return 0;
		if (n->data) return (int64_t) n->size;
		uint8_t *data = (uint8_t *) kmalloc(f->file_size);
		if (!data) return -(int64_t) 5;
		if (sqfs_read_file_range(f, data, 0, f->file_size) < 0) {
			kfree(data);
			return -(int64_t) 5;
		}
		n->data = data;
		n->capacity = f->file_size;
		return (int64_t) f->file_size;
	}

	if (!r) return 0;
	if (sqfs_read_file_range(f, (uint8_t *) buf, off, r) < 0) return -(int64_t) 5;
	return (int64_t) r;
}

static int sqfs_read_file_range(sqfs_file_t *f, uint8_t *dst, uint64_t off, uint64_t n) {
	sqfs_t *sq = f->sq;
	uint64_t done = 0;
	uint64_t pos = off;

	while (done < n) {
		uint64_t bidx = pos >> sq->block_log;
		uint64_t boff = pos - (bidx << sq->block_log);
		uint64_t brem = (uint64_t) sq->block_size - boff;
		uint64_t want = n - done;
		if (want > brem) want = brem;
		uint64_t fremain = f->file_size - pos;
		if (want > fremain) want = fremain;
		if (!want) break;

		bool in_frag = f->fragment != SQFS_INVAL_FRAG &&
		               bidx == (f->file_size >> sq->block_log);

		if (in_frag) {
			uint64_t tail = f->file_size - (bidx << sq->block_log);
			if (want > tail) want = tail;
			if (sqfs_read_fragment(sq, f->fragment, f->frag_offset + boff, dst + done, want) < 0) {
				return -1;
			}
		}
		else {
			if (bidx >= f->nblocks) return -1;
			uint64_t dpos = f->start_block;
			for (uint64_t i = 0; i < bidx; i++) {
				uint32_t raw = f->block_list[i];
				if (raw >> 25) return -1;
				dpos += raw & 0x00ffffffu;
			}
			uint32_t raw = f->block_list[bidx];
			uint64_t expect = sq->block_size;
			uint64_t fblocks = f->fragment != SQFS_INVAL_FRAG
			                       ? f->file_size >> sq->block_log
			                       : (f->file_size + sq->block_size - 1u) >> sq->block_log;
			if (bidx == fblocks - 1 && f->fragment == SQFS_INVAL_FRAG) {
				expect = f->file_size - (bidx << sq->block_log);
			}
			uint8_t *block = (uint8_t *) kmalloc(expect);
			if (!block) return -1;
			if (sqfs_read_block(sq, dpos, raw, block, expect) < 0) {
				kfree(block);
				return -1;
			}
			memcpy(dst + done, block + boff, want);
			kfree(block);
		}

		done += want;
		pos += want;
	}

	return 0;
}

static int64_t sqfs_reg_write(vfs_node_t *n, const char *buf, uint64_t off, uint64_t len) {
	(void) n;
	(void) buf;
	(void) off;
	(void) len;
	return -(int64_t) 30;
}

static int sqfs_reg_truncate(vfs_node_t *n, uint64_t len) {
	(void) n;
	(void) len;
	return -(int64_t) 30;
}

static void sqfs_reg_close(vfs_node_t *n) {
	if (n && n->fs_private) {
		kfree(n->fs_private);
		n->fs_private = NULL;
	}
}

static struct vfs_fs_ops sqfs_reg_ops = {
	.read = sqfs_reg_read,
	.write = sqfs_reg_write,
	.truncate = sqfs_reg_truncate,
	.close = sqfs_reg_close,
};

static void sqfs_free_context(sqfs_t *sq) {
	if (!sq) return;
	for (int i = 0; i < SQFS_META_CACHE_SLOTS; i++) {
		if (sq->meta[i].data) kfree(sq->meta[i].data);
	}
	if (sq->frag_index) kfree(sq->frag_index);
	if (sq->id_index) kfree(sq->id_index);
	kfree(sq);
}

static void sqfs_tree_walk(sqfs_t *sq, vfs_node_t *node) {
	if (!node) return;
	for (vfs_node_t *c = node->children; c; c = c->next) {
		if (c->fs_private && c->fs_ops == &sqfs_reg_ops) {
			sqfs_file_t *f = (sqfs_file_t *) c->fs_private;
			if (f->sq == sq) {
				kfree(c->fs_private);
				c->fs_private = NULL;
				c->fs_ops = NULL;
			}
		}
		if (c->type == VFS_TYPE_DIR) sqfs_tree_walk(sq, c);
	}
}

void squashfs_destroy(struct vfs_node *root) {
	if (!root) return;
	sqfs_t *sq = NULL;
	for (int i = 0; i < SQFS_MAX_MOUNTS; i++) {
		if (g_sqfs_mounts[i] && g_sqfs_mounts[i]->root_node == root) {
			sq = g_sqfs_mounts[i];
			break;
		}
	}
	if (!sq) return;
	sqfs_tree_walk(sq, root);
	for (int i = 0; i < SQFS_MAX_MOUNTS; i++) {
		if (g_sqfs_mounts[i] == sq) g_sqfs_mounts[i] = NULL;
	}
	sqfs_free_context(sq);
}

static int sqfs_read_sb(sqfs_t *sq, sqfs_sb_t *sb) {
	if (!sq->dev->ops || !sq->dev->ops->read) return -1;
	uint8_t buf[512];
	if (sq->dev->sector_size > sizeof(buf)) return -1;
	if (sq->dev->ops->read(sq->dev, 0, 1, buf) < 0) return -1;
	memcpy(sb, buf, sizeof(*sb));
	return 0;
}

bool squashfs_probe(struct block_device *dev) {
	if (!dev || !dev->ops || !dev->ops->read) return false;
	uint8_t buf[512];
	uint32_t ss = dev->sector_size ? dev->sector_size : 512u;
	if (ss > sizeof(buf)) return false;
	if (dev->ops->read(dev, 0, 1, buf) < 0) return false;
	uint32_t magic = (uint32_t) buf[0] | ((uint32_t) buf[1] << 8) | ((uint32_t) buf[2] << 16) |
	                 ((uint32_t) buf[3] << 24);
	uint16_t major = (uint16_t) buf[28] | ((uint16_t) buf[29] << 8);
	uint16_t minor = (uint16_t) buf[30] | ((uint16_t) buf[31] << 8);
	uint16_t comp = (uint16_t) buf[20] | ((uint16_t) buf[21] << 8);
	return magic == SQFS_MAGIC && major == SQFS_MAJOR && minor == SQFS_MINOR &&
	       (comp == SQFS_COMP_ZLIB || comp == SQFS_COMP_XZ || comp == SQFS_COMP_ZSTD);
}

static sqfs_t *sqfs_open(struct block_device *dev) {
	if (!squashfs_probe(dev)) return NULL;

	sqfs_t *sq = (sqfs_t *) kcalloc(1, sizeof(sqfs_t));
	if (!sq) return NULL;
	sq->dev = dev;

	sqfs_sb_t sb;
	if (sqfs_read_sb(sq, &sb) < 0) {
		sqfs_free_context(sq);
		return NULL;
	}

	sq->compression = sb.compression;
	sq->block_size = sb.block_size;
	sq->block_log = sb.block_log;
	sq->bytes_used = sb.bytes_used;
	sq->inode_table = sb.inode_table_start;
	sq->dir_table = sb.directory_table_start;
	sq->fragments = sb.fragments;
	sq->ids = sb.no_ids;
	sq->root_ref = sb.root_inode;

	uint64_t dev_bytes = (uint64_t) dev->sectors *
	                    (dev->sector_size ? dev->sector_size : 512u);
	if (sb.bytes_used < sizeof(sqfs_sb_t) || sb.bytes_used > dev_bytes) {
		sqfs_free_context(sq);
		return NULL;
	}
	if (sb.block_log < SQFS_MIN_BLOCK_LOG || sb.block_log > SQFS_MAX_BLOCK_LOG) {
		sqfs_free_context(sq);
		return NULL;
	}
	if (sq->block_size != (1u << sb.block_log)) {
		sqfs_free_context(sq);
		return NULL;
	}
	if (sq->inode_table >= sq->dir_table || sq->dir_table >= sq->bytes_used ||
	    sq->inode_table >= sq->bytes_used) {
		sqfs_free_context(sq);
		return NULL;
	}
	if ((sq->root_ref & 0xffffu) >= SQFS_META_SIZE) {
		sqfs_free_context(sq);
		return NULL;
	}
	if (sq->fragments > (1u << 24) || sq->ids > 65535u) {
		sqfs_free_context(sq);
		return NULL;
	}

	if (sq->ids) {
		uint32_t index_len = (sq->ids * 4 + SQFS_META_SIZE - 1) / SQFS_META_SIZE;
		uint64_t bytes = (uint64_t) index_len * 8;
		if (!sb.id_table_start || sb.id_table_start + bytes > sq->bytes_used) {
			sqfs_free_context(sq);
			return NULL;
		}
		uint8_t *buf = (uint8_t *) kmalloc(bytes);
		if (!buf) {
			sqfs_free_context(sq);
			return NULL;
		}
		if (sqfs_read_bytes(sq, sb.id_table_start, buf, bytes) < 0) {
			kfree(buf);
			sqfs_free_context(sq);
			return NULL;
		}
		sq->id_index = (uint64_t *) kmalloc(bytes);
		if (!sq->id_index) {
			kfree(buf);
			sqfs_free_context(sq);
			return NULL;
		}
		for (uint32_t i = 0; i < index_len; i++) {
			sq->id_index[i] = (uint64_t) buf[i * 8] | ((uint64_t) buf[i * 8 + 1] << 8) |
			                  ((uint64_t) buf[i * 8 + 2] << 16) | ((uint64_t) buf[i * 8 + 3] << 24) |
			                  ((uint64_t) buf[i * 8 + 4] << 32) | ((uint64_t) buf[i * 8 + 5] << 40) |
			                  ((uint64_t) buf[i * 8 + 6] << 48) | ((uint64_t) buf[i * 8 + 7] << 56);
		}
		sq->id_index_len = index_len;
		kfree(buf);
	}

	if (sq->fragments) {
		uint32_t index_len = (sq->fragments * 16 + SQFS_META_SIZE - 1) / SQFS_META_SIZE;
		uint64_t bytes = (uint64_t) index_len * 8;
		if (!sb.fragment_table_start || sb.fragment_table_start + bytes > sq->bytes_used) {
			sqfs_free_context(sq);
			return NULL;
		}
		uint8_t *buf = (uint8_t *) kmalloc(bytes);
		if (!buf) {
			sqfs_free_context(sq);
			return NULL;
		}
		if (sqfs_read_bytes(sq, sb.fragment_table_start, buf, bytes) < 0) {
			kfree(buf);
			sqfs_free_context(sq);
			return NULL;
		}
		sq->frag_index = (uint64_t *) kmalloc(bytes);
		if (!sq->frag_index) {
			kfree(buf);
			sqfs_free_context(sq);
			return NULL;
		}
		for (uint32_t i = 0; i < index_len; i++) {
			sq->frag_index[i] = (uint64_t) buf[i * 8] | ((uint64_t) buf[i * 8 + 1] << 8) |
			                    ((uint64_t) buf[i * 8 + 2] << 16) | ((uint64_t) buf[i * 8 + 3] << 24) |
			                    ((uint64_t) buf[i * 8 + 4] << 32) | ((uint64_t) buf[i * 8 + 5] << 40) |
			                    ((uint64_t) buf[i * 8 + 6] << 48) | ((uint64_t) buf[i * 8 + 7] << 56);
		}
		sq->frag_index_len = index_len;
		kfree(buf);
	}

	sqfs_ino_t root_ino;
	if (sqfs_load_inode(sq, sq->root_ref, &root_ino) < 0 ||
	    (root_ino.type != SQFS_T_DIR && root_ino.type != SQFS_T_LDIR)) {
		sqfs_ino_free(&root_ino);
		sqfs_free_context(sq);
		return NULL;
	}

	return sq;
}

static bool sqfs_mount_common(sqfs_t *sq, vfs_node_t *root_node) {
	sqfs_ino_t root_ino;
	if (sqfs_load_inode(sq, sq->root_ref, &root_ino) < 0) return false;
	if (root_ino.type != SQFS_T_DIR && root_ino.type != SQFS_T_LDIR) {
		sqfs_ino_free(&root_ino);
		return false;
	}

	root_node->uid = sqfs_id(sq, root_ino.uid_idx);
	root_node->gid = sqfs_id(sq, root_ino.gid_idx);
	root_node->mode = (root_ino.mode & 07777u) | S_IFDIR;
	root_node->ino = root_ino.ino_number;

	if (sqfs_populate_dir(sq, root_node, &root_ino, 0) < 0) {
		sqfs_ino_free(&root_ino);
		return false;
	}
	sqfs_ino_free(&root_ino);

	return true;
}

bool squashfs_mount(struct block_device *dev, const char *mount_point) {
	if (!dev || !mount_point || !mount_point[0]) return false;

	sqfs_t *sq = sqfs_open(dev);
	if (!sq) return false;

	int slot = -1;
	for (int i = 0; i < SQFS_MAX_MOUNTS; i++) {
		if (!g_sqfs_mounts[i]) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		sqfs_free_context(sq);
		return false;
	}

	size_t mplen = strlen(mount_point);
	if (mplen >= sizeof(sq->mount_point)) mplen = sizeof(sq->mount_point) - 1;
	memcpy(sq->mount_point, mount_point, mplen);
	sq->mount_point[mplen] = '\0';

	vfs_node_t *mp_node = vfs_mkdir_p(mount_point, 0755u);
	if (!mp_node) {
		sqfs_free_context(sq);
		return false;
	}

	sq->root_node = mp_node;
	if (!sqfs_mount_common(sq, mp_node)) {
		sqfs_free_context(sq);
		return false;
	}

	g_sqfs_mounts[slot] = sq;
	log_info("squashfs: mounted %s at %s  (nodes=%u frags=%u block=%u)", dev->name, mount_point,
	         sq->node_count, sq->fragments, sq->block_size);
	return true;
}

struct vfs_node *squashfs_build_root(struct block_device *dev) {
	if (!dev) return NULL;

	int slot = -1;
	for (int i = 0; i < SQFS_MAX_MOUNTS; i++) {
		if (!g_sqfs_mounts[i]) {
			slot = i;
			break;
		}
	}
	if (slot < 0) return NULL;

	sqfs_t *sq = sqfs_open(dev);
	if (!sq) return NULL;

	vfs_node_t *root = vfs_create_mount_root(0755u);
	if (!root) {
		sqfs_free_context(sq);
		return NULL;
	}

	sq->root_node = root;
	if (!sqfs_mount_common(sq, root)) {
		vfs_node_unref_internal(root);
		sqfs_free_context(sq);
		return NULL;
	}

	g_sqfs_mounts[slot] = sq;

	return root;
}

static struct filesystem sqfs_fs = {
	.name = "squashfs",
	.check_root = NULL,
	.mount = squashfs_mount,
	.build_root = squashfs_build_root,
	.destroy_root = squashfs_destroy,
	.readonly = true,
};

void squashfs_init(void) {
	kxz_init();
	vfs_register_fs(&sqfs_fs);
}
