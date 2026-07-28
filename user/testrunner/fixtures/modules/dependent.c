#include "module.h"

MODULE_NAME("hello_user");
MODULE_LICENSE("IDK");
MODULE_DESCRIPTION("Kernel module dependency test fixture");
MODULE_DEPENDS("hello");

extern int hello_answer(void);

static int dependent_init(void) {
    return hello_answer() == 42 ? 0 : -1;
}

static void dependent_exit(void) {
}

module_init(dependent_init);
module_exit(dependent_exit);
