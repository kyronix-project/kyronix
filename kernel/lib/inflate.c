#include "inflate.h"
#include "string.h"

#define INF_MAX_BITS 15
#define INF_LIT_SYMS 288
#define INF_DIST_SYMS 30
#define INF_CLEN_SYMS 19
#define INF_COMBINED (INF_LIT_SYMS + 32)

typedef struct {
	const uint8_t *data;
	uint64_t size;
	uint64_t pos;
	uint32_t bitbuf;
	uint32_t bitcnt;
} bit_reader_t;

typedef struct {
	int count[INF_MAX_BITS + 1];
	int symbol[INF_LIT_SYMS];
} huff_t;

static const uint16_t len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t len_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const uint8_t clen_order[19] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int br_bits(bit_reader_t *br, int need) {
	if (need <= 0) return 0;
	if (need > 25) return -1;
	while (br->bitcnt < (uint32_t) need) {
		if (br->pos >= br->size) return -1;
		br->bitbuf |= (uint32_t) br->data[br->pos++] << br->bitcnt;
		br->bitcnt += 8;
	}
	int val = (int) (br->bitbuf & (((uint32_t) 1 << need) - 1u));
	br->bitbuf >>= need;
	br->bitcnt -= (uint32_t) need;
	return val;
}

static int build_huff(const uint8_t *lengths, int n, huff_t *h) {
	for (int i = 0; i <= INF_MAX_BITS; i++) h->count[i] = 0;
	for (int i = 0; i < n; i++) {
		int len = lengths[i];
		if (len > INF_MAX_BITS) return -1;
		h->count[len]++;
	}
	h->count[0] = 0;
	int left = 1;
	for (int len = 1; len <= INF_MAX_BITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0) return -1;
	}
	int offs[INF_MAX_BITS + 2];
	offs[1] = 0;
	for (int len = 1; len <= INF_MAX_BITS; len++) offs[len + 1] = offs[len] + h->count[len];
	for (int i = 0; i < n; i++) {
		if (lengths[i]) h->symbol[offs[lengths[i]]++] = i;
	}
	return 0;
}

static int decode_sym(bit_reader_t *br, const huff_t *h) {
	int code = 0;
	int first = 0;
	int index = 0;
	for (int len = 1; len <= INF_MAX_BITS; len++) {
		int b = br_bits(br, 1);
		if (b < 0) return -1;
		code |= b;
		int count = h->count[len];
		if (code - first < count) return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

static int build_fixed(huff_t *lit, huff_t *dist) {
	uint8_t flens[INF_LIT_SYMS];
	uint8_t dlens[INF_DIST_SYMS];
	for (int i = 0; i < 144; i++) flens[i] = 8;
	for (int i = 144; i < 256; i++) flens[i] = 9;
	for (int i = 256; i < 280; i++) flens[i] = 7;
	for (int i = 280; i < INF_LIT_SYMS; i++) flens[i] = 8;
	for (int i = 0; i < INF_DIST_SYMS; i++) dlens[i] = 5;
	if (build_huff(flens, INF_LIT_SYMS, lit) < 0) return -1;
	if (build_huff(dlens, INF_DIST_SYMS, dist) < 0) return -1;
	return 0;
}

static int build_dynamic(bit_reader_t *br, huff_t *lit, huff_t *dist) {
	int hlit = br_bits(br, 5);
	int hdist = br_bits(br, 5);
	int hclen = br_bits(br, 4);
	if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
	int nlen = hlit + 257 + hdist + 1;
	if (nlen > INF_COMBINED) return -1;

	uint8_t clen[INF_CLEN_SYMS];
	memset(clen, 0, sizeof(clen));
	for (int i = 0; i < hclen + 4; i++) {
		int v = br_bits(br, 3);
		if (v < 0) return -1;
		clen[clen_order[i]] = (uint8_t) v;
	}
	huff_t ch;
	if (build_huff(clen, INF_CLEN_SYMS, &ch) < 0) return -1;

	uint8_t lens[INF_COMBINED];
	int i = 0;
	int prev = 0;
	while (i < nlen) {
		int sym = decode_sym(br, &ch);
		if (sym < 0) return -1;
		if (sym < 16) {
			lens[i++] = (uint8_t) sym;
			prev = sym;
		} else if (sym == 16) {
			int extra = br_bits(br, 2);
			if (extra < 0) return -1;
			int rep = 3 + extra;
			if (i + rep > nlen) return -1;
			for (int k = 0; k < rep; k++) lens[i++] = (uint8_t) prev;
		} else if (sym == 17) {
			int extra = br_bits(br, 3);
			if (extra < 0) return -1;
			int rep = 3 + extra;
			if (i + rep > nlen) return -1;
			for (int k = 0; k < rep; k++) lens[i++] = 0;
			prev = 0;
		} else {
			int extra = br_bits(br, 7);
			if (extra < 0) return -1;
			int rep = 11 + extra;
			if (i + rep > nlen) return -1;
			for (int k = 0; k < rep; k++) lens[i++] = 0;
			prev = 0;
		}
	}
	if (build_huff(lens, hlit + 257, lit) < 0) return -1;
	if (build_huff(lens + hlit + 257, hdist + 1, dist) < 0) return -1;
	return 0;
}

int64_t kinflate(const uint8_t *src, uint64_t src_len, uint8_t *dst, uint64_t dst_cap) {
	if (!src || !dst) return -1;

	bit_reader_t br;
	br.data = src;
	br.size = src_len;
	br.pos = 0;
	br.bitbuf = 0;
	br.bitcnt = 0;

	uint64_t produced = 0;
	int final = 0;
	while (!final) {
		int bfinal = br_bits(&br, 1);
		int btype = br_bits(&br, 2);
		if (bfinal < 0 || btype < 0) return -1;
		final = bfinal;

		if (btype == 0) {
			int drop = (int) (br.bitcnt & 7u);
			br.bitbuf >>= drop;
			br.bitcnt -= (uint32_t) drop;
			int len = br_bits(&br, 16);
			int nlen = br_bits(&br, 16);
			if (len < 0 || nlen < 0) return -1;
			if (nlen != ((~len) & 0xFFFF)) return -1;
			if (br.pos + (uint64_t) len > src_len) return -1;
			if (produced + (uint64_t) len > dst_cap) return -1;
			memcpy(dst + produced, src + br.pos, (uint64_t) len);
			br.pos += (uint64_t) len;
			produced += (uint64_t) len;
			continue;
		}

		huff_t lit;
		huff_t dist;
		if (btype == 1) {
			if (build_fixed(&lit, &dist) < 0) return -1;
		} else if (btype == 2) {
			if (build_dynamic(&br, &lit, &dist) < 0) return -1;
		} else {
			return -1;
		}

		for (;;) {
			int sym = decode_sym(&br, &lit);
			if (sym < 0) return -1;
			if (sym < 256) {
				if (produced >= dst_cap) return -1;
				dst[produced++] = (uint8_t) sym;
			} else if (sym == 256) {
				break;
			} else {
				if (sym > 285) return -1;
				int li = sym - 257;
				int extra = br_bits(&br, len_extra[li]);
				if (extra < 0) return -1;
				uint64_t length = (uint64_t) len_base[li] + (uint64_t) extra;
				int dsym = decode_sym(&br, &dist);
				if (dsym < 0 || dsym > 29) return -1;
				int dextra = br_bits(&br, dist_extra[dsym]);
				if (dextra < 0) return -1;
				uint64_t distance = (uint64_t) dist_base[dsym] + (uint64_t) dextra;
				if (distance > produced) return -1;
				if (produced + length > dst_cap) return -1;
				uint64_t from = produced - distance;
				for (uint64_t k = 0; k < length; k++) {
					dst[produced + k] = dst[from + k];
				}
				produced += length;
			}
		}
	}
	return (int64_t) produced;
}
