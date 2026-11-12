// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
#pragma once

#include <stdint.h>

int64_t kzstd_decode(const uint8_t *src, uint64_t src_len, uint8_t *dst, uint64_t dst_cap);
