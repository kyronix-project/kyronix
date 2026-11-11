#pragma once

#include <stdint.h>

int64_t kinflate(const uint8_t *src, uint64_t src_len, uint8_t *dst, uint64_t dst_cap);
