#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "tee";
    bool append = false;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        append = true;
        first = 2;
    }
    int out_count = argc > first ? argc - first : 0;
    FILE** outs = calloc((size_t) out_count, sizeof(*outs));
    int nouts = 0, rc = 0;
    for (int i = first; i < argc; i++) {
        outs[nouts] = fopen(argv[i], append ? "ab" : "wb");
        if (!outs[nouts]) {
            kx_warn(argv[i]);
            rc = 1;
        } else
            nouts++;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
        fwrite(buf, 1, n, stdout);
        for (int i = 0; i < nouts; i++)
            fwrite(buf, 1, n, outs[i]);
    }
    for (int i = 0; i < nouts; i++)
        fclose(outs[i]);
    free(outs);
    return rc;
}
