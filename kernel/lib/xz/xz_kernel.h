/* SPDX-License-Identifier: 0BSD */
/* Kyronix glue around the vendored xz-embedded decoder (kernel/lib/xz/). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Initialize the internal CRC32 table. Must be called once before the first
 * kxz_decode(). Safe to call repeatedly.
 */
void kxz_init(void);

/*
 * Decompress one full .xz stream (as produced by squashfs-tools "-comp xz":
 * every squashfs data/metadata block is a standalone .xz container with its
 * own Stream Header/Footer) into dst.
 *
 * Single-call mode: src must contain exactly one complete stream; trailing
 * garbage after the Stream Footer is tolerated, truncated input fails.
 *
 * Returns the number of bytes written to dst on success, or -1 on any error
 * (bad format, check mismatch, output overflow, allocation failure).
 */
int64_t kxz_decode(const uint8_t *src, uint64_t src_len, uint8_t *dst, uint64_t dst_cap);
