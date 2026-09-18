#ifndef PKG_MD5_H

#define PKG_MD5_H

#include <stddef.h>
#include <stdint.h>

#include "pkg.h"

typedef struct {
    uint32_t h[4];
    uint64_t total_len;
    unsigned char block[64];
    size_t block_len;
} Md5Ctx;

void md5_init(Md5Ctx *ctx);
void md5_update(Md5Ctx *ctx, const unsigned char *data, size_t len);
void md5_final(Md5Ctx *ctx, unsigned char out[16]);
int md5_file_hex(const char *path, char out[33]);

#endif
