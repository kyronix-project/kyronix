#include "../arch/x86_64/pit.h"
#include "../mm/heap.h"
#include "lwip/opt.h"
#include <stddef.h>
#include <stdint.h>
void *malloc(size_t s) { return kmalloc((uint64_t) s); }
void free(void *p) { kfree(p); }
void *calloc(size_t n, size_t s) { return kcalloc((uint64_t) n, (uint64_t) s); }

uint32_t sys_now(void) { return (uint32_t) g_ticks; }

long strtol(const char *s, char **end, int base) {
    (void) base;
    long v = 0;
    int neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (end) *end = (char *) s;
    return neg ? -v : v;
}
