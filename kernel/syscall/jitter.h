#pragma once

#include <stdint.h>

#define SYS_anti_toctou 511

#define ANTI_TOCTOU_CTL_QUERY 0u
#define ANTI_TOCTOU_CTL_ENABLE 1u
#define ANTI_TOCTOU_CTL_DISABLE 2u
#define ANTI_TOCTOU_CTL_RESET 3u
#define ANTI_TOCTOU_CTL_STATS 4u

int64_t sys_anti_toctou(uint32_t action, void *out);
