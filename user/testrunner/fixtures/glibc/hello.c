#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    printf("glibc hello: argc=%d argv1=%s\n", argc - 1, argc > 1 ? argv[1] : "(none)");
    const char *msg = getenv("GLIBC_MSG");
    if (msg) printf("env msg: %s\n", msg);
    const char *ec = getenv("GLIBC_EXIT");
    return ec ? atoi(ec) : 0;
}
