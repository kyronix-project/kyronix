/* SPDX-License-Identifier: 0BSD */
/*
 * Kyronix glue around the vendored xz-embedded decoder.
 *
 * squashfs-tools "-comp xz" stores every data and metadata block as a
 * standalone .xz container (Stream Header + one Block + Stream Footer), so
 * single-call decoding of exactly that byte range is all we ever need here.
 */

#include "xz_kernel.h"

#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "xz.h"

void kxz_init(void) { xz_crc32_init(); }

int64_t kxz_decode(const uint8_t *src, uint64_t src_len, uint8_t *dst, uint64_t dst_cap) {
	if (!src || !dst || src_len < 24 || dst_cap == 0) return -1;

	/* .xz Stream Header magic: FD 37 7A 58 5A 00 */
	if (memcmp(src, "\xfd\x37\x7a\x58\x5a\x00", 6) != 0) return -1;

	struct xz_dec *s = xz_dec_init(XZ_SINGLE, 0);
	if (!s) return -1;

	struct xz_buf b = {
		.in = src,
		.in_pos = 0,
		.in_size = (size_t) src_len,
		.out = dst,
		.out_pos = 0,
		.out_size = (size_t) dst_cap,
	};

	enum xz_ret ret = xz_dec_run(s, &b);
	xz_dec_end(s);

	if (ret != XZ_STREAM_END) return -1;
	return (int64_t) b.out_pos;
}
