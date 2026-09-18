#include "blockdev.h"
#include "../fs/vfs.h"
#include "../fs/vfs_internal.h"
#include "../lib/log.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../syscall/syscall.h"
#include "block.h"

#define EBADF 9
#define EFAULT 14
#define EINVAL 22
#define ENOMEM 12
#define ENOSPC 28

#define BLKGETSIZE   0x1260
#define BLKGETSIZE64 0x80081272
#define BLKSSZGET    0x1268
#define BLKFLSBUF    0x1261
#define BLKDISCARD   0x127F

#define BLK_IO_MAX (16ULL * 1024 * 1024)

static struct block_device *node_to_blk(vfs_node_t *n) {
    return (struct block_device *) n->fs_private;
}

static int64_t blk_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t pos) {
    struct block_device *bd = node_to_blk(n);
    if (!bd || !bd->ops) return -(int64_t) EINVAL;
    if (len == 0) return 0;

    if (!bd->sector_size) return -(int64_t) EINVAL;
    uint64_t total_bytes = (uint64_t) bd->sectors * bd->sector_size;
    if (pos >= total_bytes) return 0;
    if (pos + len > total_bytes) len = total_bytes - pos;
    if (len > BLK_IO_MAX) len = BLK_IO_MAX;
    if (!uptr_ok_w(buf, len)) return -(int64_t) EFAULT;

    uint64_t lba = pos / bd->sector_size;
    uint32_t count = (uint32_t) ((len + bd->sector_size - 1) / bd->sector_size);
    uint64_t off_in_first = pos % bd->sector_size;

    uint32_t buf_size = count * bd->sector_size;
    uint8_t *kbuf = (uint8_t *) kmalloc(buf_size);
    if (!kbuf) return -(int64_t) ENOMEM;

    int r = bd->ops->read(bd, lba + bd->offset_lba, count, kbuf);
    if (r < 0) {
        kfree(kbuf);
        return -(int64_t) EINVAL;
    }

    uint64_t avail = buf_size - off_in_first;
    uint64_t copy = (len < avail) ? len : avail;
    memcpy(buf, kbuf + off_in_first, copy);
    kfree(kbuf);
    return (int64_t) copy;
}

static int64_t blk_write(vfs_node_t *n, const char *buf, uint64_t len, uint64_t pos) {
    struct block_device *bd = node_to_blk(n);
    if (!bd || !bd->ops) return -(int64_t) EINVAL;
    if (len == 0) return 0;
    if (!bd->sector_size) return -(int64_t) EINVAL;

    uint64_t total_bytes = (uint64_t) bd->sectors * bd->sector_size;
    if (pos >= total_bytes) return -(int64_t) ENOSPC;
    if (pos + len > total_bytes) len = total_bytes - pos;
    if (len > BLK_IO_MAX) len = BLK_IO_MAX;
    if (!uptr_ok(buf, len)) return -(int64_t) EFAULT;

    uint64_t lba = pos / bd->sector_size;
    uint32_t count = (uint32_t) ((len + bd->sector_size - 1) / bd->sector_size);
    uint64_t off_in_first = pos % bd->sector_size;

    if (off_in_first != 0 || (len % bd->sector_size) != 0) {
        uint32_t total = count * bd->sector_size;
        uint8_t *kbuf = (uint8_t *) kmalloc(total);
        if (!kbuf) return -(int64_t) ENOMEM;

        if (off_in_first > 0) {
            int r = bd->ops->read(bd, lba + bd->offset_lba, 1, kbuf);
            if (r < 0) {
                kfree(kbuf);
                return -(int64_t) EINVAL;
            }
        }
        if (count > 1) {
            uint64_t last_lba = lba + count - 1;
            uint64_t last_off = total - bd->sector_size;
            if (last_off != off_in_first) {
                int r = bd->ops->read(bd, last_lba + bd->offset_lba, 1,
                                      kbuf + last_off);
                if (r < 0) {
                    kfree(kbuf);
                    return -(int64_t) EINVAL;
                }
            }
        }

        memcpy(kbuf + off_in_first, buf, len);
        int r = bd->ops->write(bd, lba + bd->offset_lba, count, kbuf);
        kfree(kbuf);
        if (r < 0) return -(int64_t) EINVAL;
    } else {
        uint8_t *kbuf = (uint8_t *) kmalloc(len);
        if (!kbuf) return -(int64_t) ENOMEM;
        memcpy(kbuf, buf, len);
        int r = bd->ops->write(bd, lba + bd->offset_lba, count, kbuf);
        kfree(kbuf);
        if (r < 0) return -(int64_t) EINVAL;
    }

    return (int64_t) len;
}

static int64_t blk_ioctl(vfs_node_t *n, uint64_t req, uint64_t arg) {
    struct block_device *bd = node_to_blk(n);
    if (!bd) return -(int64_t) EINVAL;

    switch (req) {
    case BLKGETSIZE: {
        uint64_t size = (uint64_t) bd->sectors * bd->sector_size;
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(uint64_t)))
            return -(int64_t) EFAULT;
        *(uint64_t *) (uintptr_t) arg = size;
        return 0;
    }
    case BLKGETSIZE64: {
        uint64_t size = (uint64_t) bd->sectors * bd->sector_size;
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(uint64_t)))
            return -(int64_t) EFAULT;
        *(uint64_t *) (uintptr_t) arg = size;
        return 0;
    }
    case BLKSSZGET: {
        if (!uptr_ok_w((void *) (uintptr_t) arg, sizeof(int)))
            return -(int64_t) EFAULT;
        *(int *) (uintptr_t) arg = (int) bd->sector_size;
        return 0;
    }
    case BLKFLSBUF: {
        if (bd->ops->flush) return bd->ops->flush(bd);
        return 0;
    }
    default:
        return -(int64_t) EINVAL;
    }
}

static void blockdev_register_one(struct block_device *bd) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/%s", bd->name);

    vfs_node_t *n = vfs_create_chr(path, blk_read, blk_write);
    if (!n) {
        log_warn("blockdev: failed to create %s", path);
        return;
    }
    n->mode = S_IFCHR | 0600;
    n->chr_ioctl = blk_ioctl;
    n->fs_private = bd;
    n->size = (uint64_t) bd->sectors * bd->sector_size;

    log_info("blockdev: %s  %lu sectors  (%lu MiB)", path, bd->sectors,
             bd->sectors / 2048);
}

void blockdev_init(void) {
    vfs_mkdir_p("/dev", 0755);
}

void blockdev_create_all(void) {
    for (int i = 0; i < block_count(); i++) {
        struct block_device *bd = block_get(i);
        if (bd) blockdev_register_one(bd);
    }
}
