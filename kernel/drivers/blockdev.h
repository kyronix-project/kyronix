#pragma once

#include <stdbool.h>
#include <stdint.h>

struct block_device;

void blockdev_init(void);
void blockdev_create_all(void);
void blockdev_create_node(struct block_device *bd);
void blockdev_remove_node(struct block_device *bd);
