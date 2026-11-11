#pragma once
#include <stdbool.h>
#include <stdint.h>

struct block_device;
struct vfs_node;

void squashfs_init(void);
bool squashfs_probe(struct block_device *dev);
bool squashfs_mount(struct block_device *dev, const char *mount_point);
struct vfs_node *squashfs_build_root(struct block_device *dev);
void squashfs_destroy(struct vfs_node *root);
