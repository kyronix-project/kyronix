#include "partition.h"
#include "../drivers/block.h"
#include "../drivers/blockdev.h"
#include "../lib/log.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../mm/heap.h"

#define MBR_SIGNATURE_OFFSET 510
#define MBR_SIGNATURE_MAGIC   0xAA55
#define MBR_PARTITION_ENTRIES_OFFSET 446
#define MBR_PARTITION_ENTRY_SIZE 16
#define MBR_NUM_PARTITIONS 4

typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_first;
    uint32_t sector_count;
} mbr_entry_t;

static int partition_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    struct block_device *parent = dev->parent;
    return parent->ops->read(parent, dev->offset_lba + lba, count, buf);
}

static int partition_write(struct block_device *dev, uint64_t lba, uint32_t count,
                           const void *buf) {
    struct block_device *parent = dev->parent;
    return parent->ops->write(parent, dev->offset_lba + lba, count, buf);
}

static int partition_flush(struct block_device *dev) {
    struct block_device *parent = dev->parent;
    if (parent->ops->flush) return parent->ops->flush(parent);
    return 0;
}

static void scan_disk(struct block_device *bd) {
    if (bd->parent) return;

    uint8_t *sector = (uint8_t *) kmalloc(bd->sector_size);
    if (!sector) return;

    if (bd->ops->read(bd, 0, 1, sector) < 0) {
        kfree(sector);
        return;
    }

    uint16_t sig = (uint16_t) sector[MBR_SIGNATURE_OFFSET] |
                   ((uint16_t) sector[MBR_SIGNATURE_OFFSET + 1] << 8);
    if (sig != MBR_SIGNATURE_MAGIC) {
        kfree(sector);
        return;
    }

    mbr_entry_t *entries = (mbr_entry_t *) (sector + MBR_PARTITION_ENTRIES_OFFSET);
    static struct block_device_ops part_ops = {
        partition_read,
        partition_write,
        partition_flush,
    };

    for (int i = 0; i < MBR_NUM_PARTITIONS; i++) {
        mbr_entry_t *e = &entries[i];
        if (e->type == 0x00) continue;
        if (e->sector_count == 0) continue;

        struct block_device *pd = (struct block_device *) kmalloc(sizeof(struct block_device));
        if (!pd) continue;

        memset(pd, 0, sizeof(*pd));
        snprintf(pd->name, sizeof(pd->name), "%s%d", bd->name, i + 1);
        pd->sectors = e->sector_count;
        pd->sector_size = bd->sector_size;
        pd->ops = &part_ops;
        pd->priv = bd->priv;
        pd->offset_lba = e->lba_first;
        pd->parent = bd;

        block_register(pd);

        log_info("partition: %s  type=0x%02x  lba=%u  sectors=%u", pd->name,
                 e->type, e->lba_first, e->sector_count);
    }

    kfree(sector);
}

bool partition_rescan_disk(struct block_device *disk) {
    if (!disk || disk->parent) return false;

    for (int i = 0; i < block_count();) {
        struct block_device *child = block_get(i);
        if (!child || child->parent != disk) {
            i++;
            continue;
        }
        blockdev_remove_node(child);
        block_unregister(child);
        kfree(child);
    }

    int before = block_count();
    scan_disk(disk);
    for (int i = before; i < block_count(); i++) {
        struct block_device *child = block_get(i);
        if (child && child->parent == disk) blockdev_create_node(child);
    }
    return true;
}

void partition_scan_all(void) {
    int count = block_count();
    for (int i = 0; i < count; i++) {
        struct block_device *bd = block_get(i);
        if (bd) scan_disk(bd);
    }
}
