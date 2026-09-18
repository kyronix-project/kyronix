#pragma once

#include <stdbool.h>
#include <stdint.h>

struct block_device;

void blockdev_init(void);
void blockdev_create_all(void);
