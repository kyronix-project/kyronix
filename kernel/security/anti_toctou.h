#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ANTI_TOCTOU_VFS_CHECK 0x01u
#define ANTI_TOCTOU_VFS_USE 0x02u
#define ANTI_TOCTOU_VFS_WRITE 0x04u

#define ANTI_TOCTOU_MEM_WAIT 0x01u
#define ANTI_TOCTOU_MEM_WAKE 0x02u
#define ANTI_TOCTOU_MEM_PROTECT 0x04u

#define ANTI_TOCTOU_MIN_DELAY_US 25u
#define ANTI_TOCTOU_MAX_DELAY_US 250u

typedef struct {
    uint32_t enabled;
    uint32_t min_delay_us;
    uint32_t max_delay_us;
    uint32_t queue_capacity;
    uint64_t vfs_observations;
    uint64_t memory_observations;
    uint64_t vfs_detections;
    uint64_t memory_detections;
    uint64_t delays_queued;
    uint64_t delays_applied;
    uint64_t queue_drops;
    uint64_t total_delay_us;
    uint32_t last_delay_us;
    uint32_t reserved;
} anti_toctou_stats_t;

bool anti_toctou_init(void);
void anti_toctou_set_enabled(bool enabled);
bool anti_toctou_enabled(void);
void anti_toctou_reset(void);
void anti_toctou_get_stats(anti_toctou_stats_t *out);
void anti_toctou_observe_vfs(uint64_t resource, uint32_t operation);
void anti_toctou_observe_memory(uint64_t address, uint32_t operation);
void anti_toctou_safe_point(void);
