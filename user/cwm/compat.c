#include <string.h>
#include <stdint.h>

void *__memcpy_chk(void *dest, const void *src, size_t n, size_t destlen)
{
    (void)destlen;
    return memcpy(dest, src, n);
}

void *__memset_chk(void *s, int c, size_t n, size_t destlen)
{
    (void)destlen;
    return memset(s, c, n);
}
