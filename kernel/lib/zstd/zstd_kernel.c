// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "zstd_kernel.h"

#define ZSTD_DEPS_MALLOC
#define ZSTD_malloc(s) kmalloc((uint64_t) (s))
#define ZSTD_calloc(n, s) kcalloc((uint64_t) (n), (uint64_t) (s))
#define ZSTD_free(p) kfree(p)
#define XXH_NO_STDLIB

#include "zstddeclib.c"

int64_t kzstd_decode(const uint8_t *src, uint64_t src_len, uint8_t *dst, uint64_t dst_cap) {
	if (!src || !dst || src_len == 0 || dst_cap == 0) return -1;

	size_t n = ZSTD_decompress(dst, (size_t) dst_cap, src, (size_t) src_len);
	if (ZSTD_isError(n)) return -1;
	return (int64_t) n;
}
