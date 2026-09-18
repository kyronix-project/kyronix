#include "wpa.h"
#include "../../crypto/chacha20.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    uint32_t buflen;
} sha1_ctx_t;

static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(sha1_ctx_t *c, const uint8_t *b) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t) b[i * 4] << 24) | ((uint32_t) b[i * 4 + 1] << 16) |
               ((uint32_t) b[i * 4 + 2] << 8) | b[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b_ = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b_ & cc) | (~b_ & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b_ ^ cc ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b_ & cc) | (b_ & d) | (cc & d);
            k = 0x8F1BBCDC;
        } else {
            f = b_ ^ cc ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = rol32(b_, 30);
        b_ = a;
        a = tmp;
    }
    c->h[0] += a;
    c->h[1] += b_;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_init(sha1_ctx_t *c) {
    c->h[0] = 0x67452301;
    c->h[1] = 0xEFCDAB89;
    c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476;
    c->h[4] = 0xC3D2E1F0;
    c->len = 0;
    c->buflen = 0;
}

static void sha1_update(sha1_ctx_t *c, const uint8_t *data, uint32_t len) {
    c->len += len;
    while (len > 0) {
        uint32_t space = 64 - c->buflen;
        uint32_t take = len < space ? len : space;
        memcpy(c->buf + c->buflen, data, take);
        c->buflen += take;
        data += take;
        len -= take;
        if (c->buflen == 64) {
            sha1_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

static void sha1_final(sha1_ctx_t *c, uint8_t *out) {
    uint64_t bitlen = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buflen != 56) sha1_update(c, &zero, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[7 - i] = (uint8_t) (bitlen >> (i * 8));
    sha1_update(c, lenbuf, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t) (c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t) (c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t) (c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t) c->h[i];
    }
}

void sha1(const uint8_t *data, uint32_t len, uint8_t *digest) {
    sha1_ctx_t c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, digest);
}

void hmac_sha1(const uint8_t *key, uint32_t key_len, const uint8_t *data, uint32_t len,
               uint8_t *digest) {
    uint8_t k[64];
    memset(k, 0, 64);
    if (key_len > 64) {
        sha1(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }
    uint8_t inner[20];
    sha1_ctx_t c;
    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    sha1_update(&c, data, len);
    sha1_final(&c, inner);
    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, digest);
}

void pbkdf2_sha1(const char *password, const uint8_t *ssid, uint32_t ssid_len, int iterations,
                 uint8_t *out, uint32_t out_len) {
    uint32_t pass_len = 0;
    while (password[pass_len]) pass_len++;
    uint8_t *salt = kmalloc(ssid_len + 4);
    if (!salt) return;
    memcpy(salt, ssid, ssid_len);
    uint32_t blocks = (out_len + 19) / 20;
    for (uint32_t blk = 1; blk <= blocks; blk++) {
        salt[ssid_len] = (uint8_t) (blk >> 24);
        salt[ssid_len + 1] = (uint8_t) (blk >> 16);
        salt[ssid_len + 2] = (uint8_t) (blk >> 8);
        salt[ssid_len + 3] = (uint8_t) blk;
        uint8_t u[20], t[20];
        hmac_sha1((const uint8_t *) password, pass_len, salt, ssid_len + 4, u);
        memcpy(t, u, 20);
        for (int it = 1; it < iterations; it++) {
            hmac_sha1((const uint8_t *) password, pass_len, u, 20, u);
            for (int i = 0; i < 20; i++) t[i] ^= u[i];
        }
        uint32_t off = (blk - 1) * 20;
        uint32_t copy = out_len - off;
        if (copy > 20) copy = 20;
        memcpy(out + off, t, copy);
    }
    kfree(salt);
}

static const uint8_t aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab,
    0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4,
    0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71,
    0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6,
    0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb,
    0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45,
    0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44,
    0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a,
    0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49,
    0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25,
    0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e,
    0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1,
    0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb,
    0x16,
};

static const uint8_t aes_inv_sbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7,
    0xfb, 0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde,
    0xe9, 0xcb, 0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42,
    0xfa, 0xc3, 0x4e, 0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49,
    0x6d, 0x8b, 0xd1, 0x25, 0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c,
    0xcc, 0x5d, 0x65, 0xb6, 0x92, 0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15,
    0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84, 0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7,
    0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06, 0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02,
    0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b, 0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc,
    0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73, 0x96, 0xac, 0x74, 0x22, 0xe7, 0xad,
    0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e, 0x47, 0xf1, 0x1a, 0x71, 0x1d,
    0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b, 0xfc, 0x56, 0x3e, 0x4b,
    0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4, 0x1f, 0xdd, 0xa8,
    0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f, 0x60, 0x51,
    0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef, 0xa0,
    0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c,
    0x7d,
};

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1B;
        b >>= 1;
    }
    return r;
}

static void aes_key_expand(const uint8_t *key, uint8_t *rk) {
    memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 1; i < 11; i++) {
        uint8_t *prev = rk + (i - 1) * 16;
        uint8_t *cur = rk + i * 16;
        uint8_t t[4] = {prev[13], prev[14], prev[15], prev[12]};
        for (int j = 0; j < 4; j++) t[j] = aes_sbox[t[j]];
        t[0] ^= rcon;
        for (int j = 0; j < 4; j++) cur[j] = prev[j] ^ t[j];
        for (int j = 4; j < 16; j++) cur[j] = cur[j - 4] ^ prev[j];
        rcon = gf_mul(rcon, 2);
    }
}

static void aes_add_round_key(uint8_t *s, const uint8_t *rk) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}

static void aes_sub_bytes(uint8_t *s) {
    for (int i = 0; i < 16; i++) s[i] = aes_sbox[s[i]];
}

static void aes_inv_sub_bytes(uint8_t *s) {
    for (int i = 0; i < 16; i++) s[i] = aes_inv_sbox[s[i]];
}

static void aes_shift_rows(uint8_t *s) {
    uint8_t t;
    t = s[1];
    s[1] = s[5];
    s[5] = s[9];
    s[9] = s[13];
    s[13] = t;
    t = s[2];
    s[2] = s[10];
    s[10] = t;
    t = s[6];
    s[6] = s[14];
    s[14] = t;
    t = s[15];
    s[15] = s[11];
    s[11] = s[7];
    s[7] = s[3];
    s[3] = t;
}

static void aes_inv_shift_rows(uint8_t *s) {
    uint8_t t;
    t = s[13];
    s[13] = s[9];
    s[9] = s[5];
    s[5] = s[1];
    s[1] = t;
    t = s[2];
    s[2] = s[10];
    s[10] = t;
    t = s[6];
    s[6] = s[14];
    s[14] = t;
    t = s[3];
    s[3] = s[7];
    s[7] = s[11];
    s[11] = s[15];
    s[15] = t;
}

static void aes_mix_columns(uint8_t *s) {
    for (int i = 0; i < 4; i++) {
        uint8_t *c = s + i * 4;
        uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
        c[0] = gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3;
        c[1] = a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3;
        c[2] = a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3);
        c[3] = gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2);
    }
}

static void aes_inv_mix_columns(uint8_t *s) {
    for (int i = 0; i < 4; i++) {
        uint8_t *c = s + i * 4;
        uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
        c[0] = gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9);
        c[1] = gf_mul(a0, 9) ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13);
        c[2] = gf_mul(a0, 13) ^ gf_mul(a1, 9) ^ gf_mul(a2, 14) ^ gf_mul(a3, 11);
        c[3] = gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9) ^ gf_mul(a3, 14);
    }
}

void aes128_encrypt_block(const uint8_t *key, const uint8_t *in, uint8_t *out) {
    uint8_t rk[176];
    aes_key_expand(key, rk);
    uint8_t s[16];
    memcpy(s, in, 16);
    aes_add_round_key(s, rk);
    for (int round = 1; round < 10; round++) {
        aes_sub_bytes(s);
        aes_shift_rows(s);
        aes_mix_columns(s);
        aes_add_round_key(s, rk + round * 16);
    }
    aes_sub_bytes(s);
    aes_shift_rows(s);
    aes_add_round_key(s, rk + 160);
    memcpy(out, s, 16);
}

void aes128_decrypt_block(const uint8_t *key, const uint8_t *in, uint8_t *out) {
    uint8_t rk[176];
    aes_key_expand(key, rk);
    uint8_t s[16];
    memcpy(s, in, 16);
    aes_add_round_key(s, rk + 160);
    for (int round = 9; round >= 1; round--) {
        aes_inv_shift_rows(s);
        aes_inv_sub_bytes(s);
        aes_add_round_key(s, rk + round * 16);
        aes_inv_mix_columns(s);
    }
    aes_inv_shift_rows(s);
    aes_inv_sub_bytes(s);
    aes_add_round_key(s, rk);
    memcpy(out, s, 16);
}

void aes_key_wrap_unwrap(const uint8_t *kek, const uint8_t *wrapped, int wrapped_len, uint8_t *out,
                         int *out_len) {
    int n = wrapped_len / 8 - 1;
    if (n < 1) {
        *out_len = 0;
        return;
    }
    uint8_t a[8];
    memcpy(a, wrapped, 8);
    uint8_t r[16][8];
    for (int i = 0; i < n; i++) memcpy(r[i], wrapped + 8 + i * 8, 8);
    for (int j = 5; j >= 0; j--) {
        for (int i = n; i >= 1; i--) {
            uint8_t buf[16];
            memcpy(buf, a, 8);
            uint64_t t = (uint64_t) (n * j + i);
            for (int k = 0; k < 8; k++) buf[k] ^= (uint8_t) (t >> (56 - k * 8));
            memcpy(buf + 8, r[i - 1], 8);
            uint8_t dec[16];
            aes128_decrypt_block(kek, buf, dec);
            memcpy(a, dec, 8);
            memcpy(r[i - 1], dec + 8, 8);
        }
    }
    for (int i = 0; i < n; i++) memcpy(out + i * 8, r[i], 8);
    *out_len = n * 8;
}

static void ccmp_nonce(uint8_t *nonce, const uint8_t *a2, uint64_t pn, uint8_t priority) {
    nonce[0] = priority;
    memcpy(nonce + 1, a2, 6);
    for (int i = 0; i < 6; i++) nonce[7 + i] = (uint8_t) (pn >> (i * 8));
}

static void ccmp_mic(const uint8_t *tk, const uint8_t *nonce, const uint8_t *aad, int aad_len,
                     const uint8_t *data, int data_len, uint8_t *mic) {
    uint8_t b0[16];
    b0[0] = 0x59;
    memcpy(b0 + 1, nonce, 13);
    b0[14] = (uint8_t) (data_len >> 8);
    b0[15] = (uint8_t) data_len;

    uint8_t x[16];
    aes128_encrypt_block(tk, b0, x);

    uint8_t aad_hdr[2] = {(uint8_t) (aad_len >> 8), (uint8_t) aad_len};
    uint8_t buf[16];
    memset(buf, 0, 16);
    memcpy(buf, aad_hdr, 2);
    int aad_off = 0;
    int buf_len = 2;
    while (aad_off < aad_len) {
        int take = aad_len - aad_off;
        if (take > 16 - buf_len) take = 16 - buf_len;
        memcpy(buf + buf_len, aad + aad_off, take);
        buf_len += take;
        aad_off += take;
        if (buf_len == 16) {
            for (int i = 0; i < 16; i++) x[i] ^= buf[i];
            aes128_encrypt_block(tk, x, x);
            buf_len = 0;
            memset(buf, 0, 16);
        }
    }
    if (buf_len > 0) {
        for (int i = 0; i < 16; i++) x[i] ^= buf[i];
        aes128_encrypt_block(tk, x, x);
    }

    int data_off = 0;
    while (data_off < data_len) {
        int take = data_len - data_off;
        if (take > 16) take = 16;
        memset(buf, 0, 16);
        memcpy(buf, data + data_off, take);
        for (int i = 0; i < 16; i++) x[i] ^= buf[i];
        aes128_encrypt_block(tk, x, x);
        data_off += take;
    }

    uint8_t s0[16];
    uint8_t ctr[16];
    ctr[0] = 0x01;
    memcpy(ctr + 1, nonce, 13);
    ctr[14] = 0;
    ctr[15] = 0;
    aes128_encrypt_block(tk, ctr, s0);
    for (int i = 0; i < 8; i++) mic[i] = x[i] ^ s0[i];
}

void aes_ccmp_encrypt(const uint8_t *tk, const uint8_t *nonce, const uint8_t *aad, int aad_len,
                      uint8_t *data, int data_len, uint8_t *mic) {
    ccmp_mic(tk, nonce, aad, aad_len, data, data_len, mic);
    uint8_t ctr[16];
    ctr[0] = 0x01;
    memcpy(ctr + 1, nonce, 13);
    uint16_t counter = 1;
    int off = 0;
    while (off < data_len) {
        ctr[14] = (uint8_t) (counter >> 8);
        ctr[15] = (uint8_t) counter;
        uint8_t s[16];
        aes128_encrypt_block(tk, ctr, s);
        int take = data_len - off;
        if (take > 16) take = 16;
        for (int i = 0; i < take; i++) data[off + i] ^= s[i];
        off += take;
        counter++;
    }
}

int aes_ccmp_decrypt(const uint8_t *tk, const uint8_t *nonce, const uint8_t *aad, int aad_len,
                     uint8_t *data, int data_len, const uint8_t *mic) {
    uint8_t ctr[16];
    ctr[0] = 0x01;
    memcpy(ctr + 1, nonce, 13);
    uint16_t counter = 1;
    int off = 0;
    uint8_t *plain = kmalloc(data_len);
    if (!plain) return -1;
    memcpy(plain, data, data_len);
    while (off < data_len) {
        ctr[14] = (uint8_t) (counter >> 8);
        ctr[15] = (uint8_t) counter;
        uint8_t s[16];
        aes128_encrypt_block(tk, ctr, s);
        int take = data_len - off;
        if (take > 16) take = 16;
        for (int i = 0; i < take; i++) plain[off + i] ^= s[i];
        off += take;
        counter++;
    }
    uint8_t computed_mic[8];
    ccmp_mic(tk, nonce, aad, aad_len, plain, data_len, computed_mic);
    int ok = 1;
    for (int i = 0; i < 8; i++)
        if (computed_mic[i] != mic[i]) ok = 0;
    if (ok) memcpy(data, plain, data_len);
    kfree(plain);
    return ok ? 0 : -1;
}

void wpa_derive_psk(wpa_ctx_t *w, const char *passphrase, const uint8_t *ssid,
                    uint32_t ssid_len) {
    pbkdf2_sha1(passphrase, ssid, ssid_len, 4096, w->psk, WPA_PSK_LEN);
}

void wpa_gen_snonce(wpa_ctx_t *w) {
    chacha20_rng_bytes(&g_chacha20_rng, w->snonce, WPA_NONCE_LEN);
}

static void wpa_prf(const uint8_t *key, const char *label, const uint8_t *data, int data_len,
                    uint8_t *out, int out_len) {
    uint8_t *buf = kmalloc(data_len + 64);
    if (!buf) return;
    int label_len = 0;
    while (label[label_len]) label_len++;
    int pos = 0;
    for (uint8_t i = 0; pos < out_len; i++) {
        memcpy(buf, label, label_len);
        buf[label_len] = 0;
        memcpy(buf + label_len + 1, data, data_len);
        buf[label_len + 1 + data_len] = i;
        uint8_t digest[20];
        hmac_sha1(key, 32, buf, label_len + 2 + data_len, digest);
        int take = out_len - pos;
        if (take > 20) take = 20;
        memcpy(out + pos, digest, take);
        pos += take;
    }
    kfree(buf);
}

void wpa_derive_ptk(wpa_ctx_t *w) {
    uint8_t data[6 + 6 + WPA_NONCE_LEN + WPA_NONCE_LEN];
    const uint8_t *mac1 = w->ap_mac, *mac2 = w->sta_mac;
    if (memcmp(w->sta_mac, w->ap_mac, 6) < 0) {
        mac1 = w->sta_mac;
        mac2 = w->ap_mac;
    }
    const uint8_t *nonce1 = w->anonce, *nonce2 = w->snonce;
    if (memcmp(w->snonce, w->anonce, WPA_NONCE_LEN) < 0) {
        nonce1 = w->snonce;
        nonce2 = w->anonce;
    }
    memcpy(data, mac1, 6);
    memcpy(data + 6, mac2, 6);
    memcpy(data + 12, nonce1, WPA_NONCE_LEN);
    memcpy(data + 12 + WPA_NONCE_LEN, nonce2, WPA_NONCE_LEN);
    wpa_prf(w->psk, "Pairwise key expansion", data, sizeof(data), w->ptk, WPA_PTK_LEN);
    w->ptk_ok = true;
}

static uint32_t eapol_key_frame_len(const uint8_t *m) {
    if (!m) return 0;
    return 4 + ((uint32_t) m[2] << 8 | m[3]);
}

int wpa_build_eapol_m2(wpa_ctx_t *w, const uint8_t *m1, int m1_len, uint8_t *out, int *out_len) {
    if (m1_len < 99) return -1;
    memcpy(w->anonce, m1 + 13, WPA_NONCE_LEN);
    wpa_gen_snonce(w);
    wpa_derive_ptk(w);

    uint8_t rsn_ie[24];
    rsn_ie[0] = 0x30;
    rsn_ie[1] = 22;
    rsn_ie[2] = 0x01;
    rsn_ie[3] = 0x00;
    rsn_ie[4] = 0x00;
    rsn_ie[5] = 0x0F;
    rsn_ie[6] = 0xAC;
    rsn_ie[7] = 0x04;
    rsn_ie[8] = 0x01;
    rsn_ie[9] = 0x00;
    rsn_ie[10] = 0x00;
    rsn_ie[11] = 0x0F;
    rsn_ie[12] = 0xAC;
    rsn_ie[13] = 0x04;
    rsn_ie[14] = 0x01;
    rsn_ie[15] = 0x00;
    rsn_ie[16] = 0x00;
    rsn_ie[17] = 0x0F;
    rsn_ie[18] = 0xAC;
    rsn_ie[19] = 0x02;
    rsn_ie[20] = 0x00;
    rsn_ie[21] = 0x00;
    rsn_ie[22] = 0x00;
    rsn_ie[23] = 0x00;

    int body_len = 95 + 2 + 24;
    int frame_len = 4 + body_len;
    out[0] = 0x01;
    out[1] = 0x03;
    out[2] = (uint8_t) (body_len >> 8);
    out[3] = (uint8_t) body_len;
    out[4] = 0x02;
    out[5] = 0x03;
    out[6] = 0x00;
    out[7] = 0x5F;
    out[8] = 0x02;
    out[9] = 0x00;
    out[10] = 0x8A;
    out[11] = 0x00;
    out[12] = 0x10;
    memcpy(out + 13, w->snonce, WPA_NONCE_LEN);
    memset(out + 45, 0, 16);
    memset(out + 61, 0, 8);
    memset(out + 69, 0, 16);
    memset(out + 85, 0, 8);
    out[93] = 0;
    out[94] = 0;
    out[95] = 0;
    out[96] = 22;
    memcpy(out + 97, rsn_ie + 2, 22);
    out[119] = 0;
    out[120] = 0;

    uint8_t *mic_data = kmalloc(4 + body_len);
    if (!mic_data) return -1;
    memcpy(mic_data, out, 4 + body_len);
    memset(mic_data + 81, 0, 16);
    uint8_t mic[16];
    hmac_sha1(w->ptk, WPA_KCK_LEN, mic_data, 4 + body_len, mic);
    memcpy(out + 81, mic, 16);
    kfree(mic_data);

    *out_len = frame_len;
    return 0;
}

int wpa_process_eapol_m3(wpa_ctx_t *w, const uint8_t *m3, int m3_len, uint8_t *out,
                         int *out_len) {
    if (m3_len < 99) return -1;
    uint8_t key_data_len_hi = m3[97], key_data_len_lo = m3[98];
    int key_data_len = (key_data_len_hi << 8) | key_data_len_lo;
    if (99 + key_data_len > m3_len) return -1;

    const uint8_t *key_data = m3 + 99;
    if (m3[6] & 0x01) {
        uint8_t unwrapped[64];
        int unwrapped_len;
        aes_key_wrap_unwrap(w->ptk + WPA_KCK_LEN, key_data, key_data_len, unwrapped,
                            &unwrapped_len);
        if (unwrapped_len >= 22 && unwrapped[0] == 0x30) {
            int gtk_len = unwrapped[1];
            if (gtk_len >= 16 && 2 + gtk_len <= unwrapped_len) {
                memcpy(w->gtk, unwrapped + 8, WPA_GTK_LEN);
                w->gtk_ok = true;
            }
        }
    } else {
        if (key_data_len >= 24 && key_data[0] == 0x30) {
            memcpy(w->gtk, key_data + 8, WPA_GTK_LEN);
            w->gtk_ok = true;
        }
    }

    int body_len = 95;
    int frame_len = 4 + body_len;
    out[0] = 0x01;
    out[1] = 0x03;
    out[2] = (uint8_t) (body_len >> 8);
    out[3] = (uint8_t) body_len;
    out[4] = 0x02;
    out[5] = 0x03;
    out[6] = 0x00;
    out[7] = 0x5F;
    out[8] = 0x02;
    out[9] = 0x03;
    out[10] = 0x0A;
    out[11] = 0x00;
    out[12] = 0x00;
    memcpy(out + 13, w->anonce, WPA_NONCE_LEN);
    memset(out + 45, 0, 16);
    memset(out + 61, 0, 8);
    memset(out + 69, 0, 16);
    memset(out + 85, 0, 8);
    memset(out + 93, 0, 6);

    uint8_t *mic_data = kmalloc(4 + body_len);
    if (!mic_data) return -1;
    memcpy(mic_data, out, 4 + body_len);
    memset(mic_data + 81, 0, 16);
    uint8_t mic[16];
    hmac_sha1(w->ptk, WPA_KCK_LEN, mic_data, 4 + body_len, mic);
    memcpy(out + 81, mic, 16);
    kfree(mic_data);

    *out_len = frame_len;
    return 0;
}

uint16_t wpa_build_rsn_ie(void) { return 22; }
