#include "common.h"

static uint32_t table[256];

static void init_crc(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int j = 0; j < 8; j++)
            c = (c & 0x80000000U) ? ((c << 1) ^ 0x04C11DB7U) : (c << 1);
        table[i] = c;
    }
}

static int sum_file(const char* path)
{
    FILE* f = path ? fopen(path, "rb") : stdin;
    if (!f) {
        kx_warn(path);
        return 1;
    }
    uint32_t crc = 0;
    unsigned long len = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        crc = (crc << 8) ^ table[((crc >> 24) ^ (uint8_t) c) & 0xff];
        len++;
    }
    for (unsigned long n = len; n; n >>= 8)
        crc = (crc << 8) ^ table[((crc >> 24) ^ (n & 0xff)) & 0xff];
    crc = ~crc;
    printf("%u %lu", crc, len);
    if (path)
        printf(" %s", path);
    putchar('\n');
    if (path)
        fclose(f);
    return 0;
}

int main(int argc, char** argv)
{
    kx_prog = "cksum";
    init_crc();
    if (argc == 1)
        return sum_file(NULL);
    int rc = 0;
    for (int i = 1; i < argc; i++)
        rc |= sum_file(argv[i]);
    return rc;
}
