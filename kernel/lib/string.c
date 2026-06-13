#include "string.h"

void* memset(void* dst, int c, size_t n)
{
    uint8_t* p = dst;
    while (n--)
        *p++ = (uint8_t) c;
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n)
{
    uint8_t* d = dst;
    const uint8_t* s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, size_t n)
{
    uint8_t* d = dst;
    const uint8_t* s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void* a, const void* b, size_t n)
{
    const uint8_t *pa = a, *pb = b;
    while (n--) {
        if (*pa != *pb)
            return *pa - *pb;
        pa++;
        pb++;
    }
    return 0;
}

size_t strlen(const char* s)
{
    const char* p = s;
    while (*p)
        p++;
    return p - s;
}

int strcmp(const char* a, const char* b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char) *a - (unsigned char) *b;
}

int strncmp(const char* a, const char* b, size_t n)
{
    while (n-- && *a && *a == *b) {
        a++;
        b++;
    }
    if (n == (size_t) -1)
        return 0;
    return (unsigned char) *a - (unsigned char) *b;
}

char* strcpy(char* dst, const char* src)
{
    char* d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n)
{
    char* d = dst;
    while (n && (*d++ = *src++))
        n--;
    while (n--)
        *d++ = '\0';
    return dst;
}

char* strcat(char* dst, const char* src)
{
    char* d = dst + strlen(dst);
    while ((*d++ = *src++))
        ;
    return dst;
}

char* strchr(const char* s, int c)
{
    while (*s) {
        if (*s == (char) c)
            return (char*) s;
        s++;
    }
    return (c == '\0') ? (char*) s : NULL;
}
