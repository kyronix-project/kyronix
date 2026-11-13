#include "loop.h"

#include "../fs/vfs.h"
#include "../fs/vfs_internal.h"
#include "../lib/log.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/shmem.h"
#include "../syscall/syscall.h"
#include "block.h"

#define EBADF 9
#define EFAULT 14
#define EINVAL 22
#define ENXIO 6
#define EROFS 30
#define EBUSY 16

// TODO: in the future we need in some interface for increase loop max
#define LOOP_MAX 8

typedef struct loop_state_t {
	vfs_node_t *file;
	uint64_t offset;
	uint64_t sizelimit;
} loop_state_t;

static loop_state_t g_loops[LOOP_MAX];
static struct block_device g_loop_devs[LOOP_MAX];

static int64_t loop_file_read(loop_state_t *l, void *buf, uint64_t off, uint64_t len) {
	vfs_node_t *n = l->file;

	if (!n || n->type != VFS_TYPE_REG) return -(int64_t) EINVAL;
	if (len == 0) return 0;
	if (n->shmem) return shmem_read(n->shmem, buf, off, len);
	if (n->fs_ops && n->fs_ops->read) return n->fs_ops->read(n, (char *) buf, off, len);
	if (off >= n->size) return 0;

	uint64_t avail = n->size - off;
	uint64_t r = (len < avail) ? len : avail;
	if (n->data) {
		memcpy(buf, n->data + off, r);
	}
	else {
		r = 0;
	}
	return (int64_t) r;
}

static void loop_update_geometry(struct block_device *bd, loop_state_t *l) {
	uint64_t limit = l->sizelimit ? l->sizelimit
	                              : (l->file && l->file->size > l->offset ? l->file->size - l->offset : 0);
	bd->sectors = (limit + bd->sector_size - 1) / bd->sector_size;
}

static int loop_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
	loop_state_t *l = (loop_state_t *) dev->priv;

	if (!l || !l->file) return -ENXIO;
	if (!dev->sector_size) return -EINVAL;
	if (count == 0) return 0;
	if (lba >= dev->sectors) return -EINVAL;
	if (count > dev->sectors - lba) count = (uint32_t) (dev->sectors - lba);

	uint64_t pos = l->offset + lba * dev->sector_size;
	uint64_t total = (uint64_t) count * dev->sector_size;
	int64_t r = loop_file_read(l, buf, pos, total);
	if (r < 0) return -EINVAL;
	if ((uint64_t) r < total) memset((uint8_t *) buf + r, 0, total - (uint64_t) r);
	return 0;
}

static int loop_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
	(void) dev;
	(void) lba;
	(void) count;
	(void) buf;
	return -EROFS;
}

static int loop_bind(struct block_device *bd, loop_state_t *l, int fd, uint64_t offset,
                     uint64_t sizelimit) {
	if (l->file) return -EBUSY;

	vfs_node_t *n = fd_get_node(fd);
	if (!n) return -EBADF;
	if (n->type != VFS_TYPE_REG) return -EINVAL;
	if (offset >= n->size) return -EINVAL;
	if (sizelimit && sizelimit > n->size - offset) return -EINVAL;

	vfs_node_ref_internal(n);
	l->file = n;
	l->offset = offset;
	l->sizelimit = sizelimit;
	loop_update_geometry(bd, l);

	log_info("loop: %s bound to fd %d (offset %lu limit %lu sectors %lu)", bd->name, fd, offset,
	         sizelimit, bd->sectors);
	return 0;
}

static int loop_unbind(struct block_device *bd, loop_state_t *l) {
	if (!l->file) return -ENXIO;

	vfs_node_unref_internal(l->file);
	l->file = NULL;
	l->offset = 0;
	l->sizelimit = 0;
	bd->sectors = 0;
	return 0;
}

static int loop_ioctl(struct block_device *bd, uint64_t req, uint64_t arg) {
	int idx = (int) (bd - g_loop_devs);
	if (idx < 0 || idx >= LOOP_MAX) return -EINVAL;
	loop_state_t *l = &g_loops[idx];

	switch (req) {
	case LOOP_SET_FD: {
		return loop_bind(bd, l, (int) arg, 0, 0);
	}
	case LOOP_CLR_FD: {
		return loop_unbind(bd, l);
	}
	case LOOP_CONFIGURE: {
		loop_config_t *cfg = (loop_config_t *) (uintptr_t) arg;
		if (!cfg) return -EINVAL;
		if (!uptr_ok(cfg, sizeof(*cfg))) return -EFAULT;

		loop_config_t kc;
		memcpy(&kc, cfg, sizeof(kc));
		if (kc.block_size && kc.block_size != bd->sector_size) return -EINVAL;
		return loop_bind(bd, l, kc.fd, kc.info.lo_offset, kc.info.lo_sizelimit);
	}
	case LOOP_SET_STATUS64: {
		loop_info64_t *li = (loop_info64_t *) (uintptr_t) arg;
		if (!li) return -EINVAL;
		if (!uptr_ok(li, sizeof(*li))) return -EFAULT;
		if (!l->file) return -ENXIO;

		uint64_t offset = li->lo_offset;
		uint64_t sizelimit = li->lo_sizelimit;
		if (offset >= l->file->size) return -EINVAL;
		if (sizelimit && sizelimit > l->file->size - offset) return -EINVAL;
		l->offset = offset;
		l->sizelimit = sizelimit;
		loop_update_geometry(bd, l);
		return 0;
	}
	case LOOP_GET_STATUS64: {
		loop_info64_t *li = (loop_info64_t *) (uintptr_t) arg;
		if (!li) return -EINVAL;
		if (!uptr_ok_w(li, sizeof(*li))) return -EFAULT;
		if (!l->file) return -ENXIO;

		memset(li, 0, sizeof(*li));
		li->lo_offset = l->offset;
		li->lo_sizelimit = l->sizelimit;
		li->lo_number = (uint32_t) idx;
		return 0;
	}
	default: {
		return -EINVAL;
	}
	}
}

static struct block_device_ops loop_ops = {
	.read = loop_read,
	.write = loop_write,
};

void loop_init(void) {
	for (int i = 0; i < LOOP_MAX; i++) {
		struct block_device *bd = &g_loop_devs[i];

		memset(bd, 0, sizeof(*bd));
		snprintf(bd->name, sizeof(bd->name), "loop%d", i);
		bd->sector_size = 512;
		bd->ops = &loop_ops;
		bd->ioctl = loop_ioctl;
		bd->priv = &g_loops[i];
		block_register(bd);
	}
}
