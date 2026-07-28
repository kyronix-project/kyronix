#pragma once

#include <stdbool.h>
#include <stdint.h>

struct block_device;

void partition_scan_all(void);
bool partition_rescan_disk(struct block_device *disk);
