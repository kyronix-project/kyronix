#include "string.h"

void *memset(void *dst, int c, size_t n) {
    uint8_t v = (uint8_t) c;
    uint64_t w = 0x0101010101010101ULL * v;
    void *p = dst;
    size_t q = n >> 3, r = n & 7;
    __asm__ volatile("rep stosq" : "+D"(p), "+c"(q) : "a"(w) : "memory");
    __asm__ volatile("rep stosb" : "+D"(p), "+c"(r) : "a"(w) : "memory");
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    void *d = dst;
    const void *s = src;
    size_t q = n >> 3, r = n & 7;
    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(q) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(r) : : "memory");
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    if (dst == src || n == 0) return dst;
    const uint8_t *s = src;
    uint8_t *d = dst;
    uintptr_t du = (uintptr_t) d;
    uintptr_t su = (uintptr_t) s;
    if (du < su || du - su >= n) return memcpy(dst, src, n);

    size_t r = n & 7;
    while (r--) {
        n--;
        d[n] = s[n];
    }
    const uint64_t *sq = (const uint64_t *) s;
    uint64_t *dq = (uint64_t *) d;
    size_t q = n >> 3;
    while (q--) dq[q] = sq[q];
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = a, *pb = b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++;
        pb++;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return p - s;
}

size_t strnlen(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char) *a - (unsigned char) *b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *a == *b) {
        a++;
        b++;
    }
    if (n == (size_t) -1) return 0;
    return (unsigned char) *a - (unsigned char) *b;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    while (n--) *d++ = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char) c) return (char *) s;
        s++;
    }
    return (c == '\0') ? (char *) s : NULL;
}

int atoi(const char *s) {
    int n = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+')
        s++;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}
