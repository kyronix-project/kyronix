/* SPDX-License-Identifier: 0BSD */

/*
 * Private includes and definitions
 *
 * Author: Lasse Collin <lasse.collin@tukaani.org>
 */

#ifndef XZ_PRIVATE_H
#define XZ_PRIVATE_H

#ifdef __KERNEL__
#include "../../mm/heap.h"
#include "../../lib/string.h"
#include "xz.h"

#define XZ_DEC_X86

static inline void *vmalloc(uint64_t size) { return kmalloc(size); }
static inline void vfree(void *ptr) { kfree(ptr); }

#define memeq(a, b, size) (memcmp((a), (b), (size)) == 0)
#define memzero(buf, size) memset((buf), 0, (size))

/* Linux-isms used by the upstream decoder sources. */
#define GFP_KERNEL 0
#define __always_inline inline __attribute__((always_inline))
#ifndef fallthrough
#	define fallthrough __attribute__((fallthrough))
#endif
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define min_t(type, x, y) ((type) (x) < (type) (y) ? (type) (x) : (type) (y))
#define max_t(type, x, y) ((type) (x) > (type) (y) ? (type) (x) : (type) (y))
static inline size_t xz_min_size(size_t a, size_t b) { return a < b ? a : b; }
static inline size_t xz_max_size(size_t a, size_t b) { return a > b ? a : b; }
#define min(a, b) xz_min_size((size_t) (a), (size_t) (b))
#define max(a, b) xz_max_size((size_t) (a), (size_t) (b))

/* Kyronix heap API takes a single size argument. */
static inline void *xz_kmalloc(size_t size, unsigned int flags) {
	(void) flags;
	return kmalloc((uint64_t) size);
}
#undef kmalloc
#define kmalloc(sz, flags) xz_kmalloc((sz), (flags))

static inline uint32_t get_le32(const void *p) {
	const uint8_t *b = (const uint8_t *) p;
	return (uint32_t) b[0] | ((uint32_t) b[1] << 8) | ((uint32_t) b[2] << 16) |
	       ((uint32_t) b[3] << 24);
}

static inline uint32_t get_unaligned_le32(const void *p) { return get_le32(p); }
static inline uint32_t get_unaligned_be32(const void *p) {
	const uint8_t *b = (const uint8_t *) p;
	return ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) |
	       (uint32_t) b[3];
}
static inline void put_unaligned_le32(uint32_t val, void *p) {
	uint8_t *b = (uint8_t *) p;
	b[0] = (uint8_t) val;
	b[1] = (uint8_t) (val >> 8);
	b[2] = (uint8_t) (val >> 16);
	b[3] = (uint8_t) (val >> 24);
}
static inline void put_unaligned_be32(uint32_t val, void *p) {
	uint8_t *b = (uint8_t *) p;
	b[0] = (uint8_t) (val >> 24);
	b[1] = (uint8_t) (val >> 16);
	b[2] = (uint8_t) (val >> 8);
	b[3] = (uint8_t) val;
}
#else
	/*
	 * For userspace builds, use a separate header to define the required
	 * macros and functions. This makes it easier to adapt the code into
	 * different environments and avoids clutter in the Linux kernel tree.
	 */
#	include "xz_config.h"
#endif

/* If no specific decoding mode is requested, enable support for all modes. */
#if !defined(XZ_DEC_SINGLE) && !defined(XZ_DEC_PREALLOC) \
		&& !defined(XZ_DEC_DYNALLOC)
#	define XZ_DEC_SINGLE
#	define XZ_DEC_PREALLOC
#	define XZ_DEC_DYNALLOC
#endif

/*
 * The DEC_IS_foo(mode) macros are used in "if" statements. If only some
 * of the supported modes are enabled, these macros will evaluate to true or
 * false at compile time and thus allow the compiler to omit unneeded code.
 */
#ifdef XZ_DEC_SINGLE
#	define DEC_IS_SINGLE(mode) ((mode) == XZ_SINGLE)
#else
#	define DEC_IS_SINGLE(mode) (false)
#endif

#ifdef XZ_DEC_PREALLOC
#	define DEC_IS_PREALLOC(mode) ((mode) == XZ_PREALLOC)
#else
#	define DEC_IS_PREALLOC(mode) (false)
#endif

#ifdef XZ_DEC_DYNALLOC
#	define DEC_IS_DYNALLOC(mode) ((mode) == XZ_DYNALLOC)
#else
#	define DEC_IS_DYNALLOC(mode) (false)
#endif

#if !defined(XZ_DEC_SINGLE)
#	define DEC_IS_MULTI(mode) (true)
#elif defined(XZ_DEC_PREALLOC) || defined(XZ_DEC_DYNALLOC)
#	define DEC_IS_MULTI(mode) ((mode) != XZ_SINGLE)
#else
#	define DEC_IS_MULTI(mode) (false)
#endif

/*
 * If any of the BCJ filter decoders are wanted, define XZ_DEC_BCJ.
 * XZ_DEC_BCJ is used to enable generic support for BCJ decoders.
 */
#ifndef XZ_DEC_BCJ
#	if defined(XZ_DEC_X86) || defined(XZ_DEC_POWERPC) \
			|| defined(XZ_DEC_IA64) \
			|| defined(XZ_DEC_ARM) || defined(XZ_DEC_ARMTHUMB) \
			|| defined(XZ_DEC_SPARC) || defined(XZ_DEC_ARM64) \
			|| defined(XZ_DEC_RISCV)
#		define XZ_DEC_BCJ
#	endif
#endif

/*
 * Allocate memory for LZMA2 decoder. xz_dec_lzma2_reset() must be used
 * before calling xz_dec_lzma2_run().
 */
XZ_EXTERN struct xz_dec_lzma2 *xz_dec_lzma2_create(enum xz_mode mode,
						   uint32_t dict_max);

/*
 * Decode the LZMA2 properties (one byte) and reset the decoder. Return
 * XZ_OK on success, XZ_MEMLIMIT_ERROR if the preallocated dictionary is not
 * big enough, and XZ_OPTIONS_ERROR if props indicates something that this
 * decoder doesn't support.
 */
XZ_EXTERN enum xz_ret xz_dec_lzma2_reset(struct xz_dec_lzma2 *s,
					 uint8_t props);

/* Decode raw LZMA2 stream from b->in to b->out. */
XZ_EXTERN enum xz_ret xz_dec_lzma2_run(struct xz_dec_lzma2 *s,
				       struct xz_buf *b);

/* Free the memory allocated for the LZMA2 decoder. */
XZ_EXTERN void xz_dec_lzma2_end(struct xz_dec_lzma2 *s);

#ifdef XZ_DEC_BCJ
/*
 * Allocate memory for BCJ decoders. xz_dec_bcj_reset() must be used before
 * calling xz_dec_bcj_run().
 */
XZ_EXTERN struct xz_dec_bcj *xz_dec_bcj_create(bool single_call);

/*
 * Decode the Filter ID of a BCJ filter. This implementation doesn't
 * support custom start offsets, so no decoding of Filter Properties
 * is needed. Returns XZ_OK if the given Filter ID is supported.
 * Otherwise XZ_OPTIONS_ERROR is returned.
 */
XZ_EXTERN enum xz_ret xz_dec_bcj_reset(struct xz_dec_bcj *s, uint8_t id);

/*
 * Decode raw BCJ + LZMA2 stream. This must be used only if there actually is
 * a BCJ filter in the chain. If the chain has only LZMA2, xz_dec_lzma2_run()
 * must be called directly.
 */
XZ_EXTERN enum xz_ret xz_dec_bcj_run(struct xz_dec_bcj *s,
				     struct xz_dec_lzma2 *lzma2,
				     struct xz_buf *b);

/* Free the memory allocated for the BCJ filters. */
#define xz_dec_bcj_end(s) kfree(s)
#endif

#endif
