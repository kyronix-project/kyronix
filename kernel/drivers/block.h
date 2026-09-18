#pragma once
#include <stdbool.h>
#include <stdint.h>

struct block_device;

struct block_device_ops {
    int (*read)(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct block_device *dev);
};

#define BLOCK_NAME_MAX 16

struct block_device {
    char name[BLOCK_NAME_MAX];
    uint64_t sectors;
    uint32_t sector_size;
    struct block_device_ops *ops;
    void *priv;
    uint64_t offset_lba;
    struct block_device *parent;
};

void block_init(void);
void block_register(struct block_device *dev);
int block_count(void);
struct block_device *block_get(int index);
struct block_device *block_by_name(const char *name);
struct block_device *block_first(void);
