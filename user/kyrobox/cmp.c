#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "cmp";
    if (argc != 3)
        kx_die("usage: cmp FILE1 FILE2");
    FILE *a = fopen(argv[1], "rb"), *b = fopen(argv[2], "rb");
    if (!a) {
        kx_warn(argv[1]);
        return 2;
    }
    if (!b) {
        kx_warn(argv[2]);
        fclose(a);
        return 2;
    }
    long pos = 1;
    int ca, cb;
    while ((ca = fgetc(a)) != EOF && (cb = fgetc(b)) != EOF) {
        if (ca != cb) {
            printf("%s %s differ: byte %ld\n", argv[1], argv[2], pos);
            fclose(a);
            fclose(b);
            return 1;
        }
        pos++;
    }
    cb = fgetc(b);
    fclose(a);
    fclose(b);
    return (ca == EOF && cb == EOF) ? 0 : 1;
}
