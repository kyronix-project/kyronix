#include "md5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static uint32_t rotl32(uint32_t v, uint32_t s) {
    return (v << s) | (v >> (32U - s));
}

void md5_init(Md5Ctx *ctx) {
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xefcdab89u;
    ctx->h[2] = 0x98badcfeu;
    ctx->h[3] = 0x10325476u;
    ctx->total_len = 0;
    ctx->block_len = 0;
}

static void md5_process_block(Md5Ctx *ctx, const unsigned char block[64]) {
    static const uint32_t s[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };
    static const uint32_t k[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
        0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
        0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
        0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
        0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
        0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
        0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
        0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
        0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
        0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
        0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
    };

    uint32_t a = ctx->h[0];
    uint32_t b = ctx->h[1];
    uint32_t c = ctx->h[2];
    uint32_t d = ctx->h[3];
    uint32_t m[16];

    for (int i = 0; i < 16; i++) {
        m[i] = (uint32_t)block[i * 4 + 0]
             | ((uint32_t)block[i * 4 + 1] << 8)
             | ((uint32_t)block[i * 4 + 2] << 16)
             | ((uint32_t)block[i * 4 + 3] << 24);
    }

    for (int i = 0; i < 64; i++) {
        uint32_t f;
        uint32_t g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = (uint32_t)i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (uint32_t)((5 * i + 1) % 16);
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (uint32_t)((3 * i + 5) % 16);
        } else {
            f = c ^ (b | ~d);
            g = (uint32_t)((7 * i) % 16);
        }

        uint32_t temp = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + k[i] + m[g], s[i]);
        a = temp;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
}

void md5_update(Md5Ctx *ctx, const unsigned char *data, size_t len) {
    ctx->total_len += len;

    while (len > 0) {
        size_t to_copy = 64 - ctx->block_len;
        if (to_copy > len) to_copy = len;
        memcpy(ctx->block + ctx->block_len, data, to_copy);
        ctx->block_len += to_copy;
        data += to_copy;
        len -= to_copy;

        if (ctx->block_len == 64) {
            md5_process_block(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
}

void md5_final(Md5Ctx *ctx, unsigned char out[16]) {
    uint64_t bit_len = ctx->total_len * 8u;
    ctx->block[ctx->block_len++] = 0x80;

    if (ctx->block_len > 56) {
        while (ctx->block_len < 64) ctx->block[ctx->block_len++] = 0;
        md5_process_block(ctx, ctx->block);
        ctx->block_len = 0;
    }

    while (ctx->block_len < 56) ctx->block[ctx->block_len++] = 0;
    for (int i = 0; i < 8; i++) {
        ctx->block[56 + i] = (unsigned char)((bit_len >> (8 * i)) & 0xffu);
    }
    md5_process_block(ctx, ctx->block);

    for (int i = 0; i < 4; i++) {
        out[i * 4 + 0] = (unsigned char)(ctx->h[i] & 0xffu);
        out[i * 4 + 1] = (unsigned char)((ctx->h[i] >> 8) & 0xffu);
        out[i * 4 + 2] = (unsigned char)((ctx->h[i] >> 16) & 0xffu);
        out[i * 4 + 3] = (unsigned char)((ctx->h[i] >> 24) & 0xffu);
    }
}

int md5_file_hex(const char *path, char out[33]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    Md5Ctx ctx;
    md5_init(&ctx);

    unsigned char buf[4096];
    for (;;) {
        size_t rd = fread(buf, 1, sizeof(buf), f);
        if (rd > 0) {
            md5_update(&ctx, buf, rd);
        }
        if (rd < sizeof(buf)) {
            if (ferror(f)) {
                fclose(f);
                return -1;
            }
            break;
        }
    }

    fclose(f);

    unsigned char digest[16];
    md5_final(&ctx, digest);

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hex[(digest[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[32] = '\0';
    return 0;
}
