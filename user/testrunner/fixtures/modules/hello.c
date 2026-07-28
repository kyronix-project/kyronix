#include "module.h"

MODULE_NAME("hello");
MODULE_LICENSE("IDK");
MODULE_DESCRIPTION("Kernel module loader test fixture");

int hello_answer(void) {
    return 42;
}

static int hello_init(void) {
    return 0;
}

static void hello_exit(void) {
}

module_init(hello_init);
module_exit(hello_exit);
