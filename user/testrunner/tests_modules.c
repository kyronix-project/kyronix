#include "test_harness.h"

static int delete_test_module(void) {
    return (int) syscall(SYS_delete_module, "hello", 0);
}

static int load_module_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long rc = syscall(SYS_finit_module, fd, "", 0);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return (int) rc;
}

int test_kernel_modules(void) {
    const char *path = "/lib/modules/hello.ko";
    int ok = 1;
    int loaded = 0;
    int dependent_loaded = 0;
    int fd = -1;
    unsigned char *image = NULL;

#define MODULE_CHECK(condition)                                                                   \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            if (failure_pipe[1] >= 0)                                                             \
                dprintf(failure_pipe[1], "tests_modules.c:%d: %s (errno=%d)\n", __LINE__,         \
                        #condition, errno);                                                        \
            ok = 0;                                                                               \
            goto cleanup;                                                                         \
        }                                                                                         \
    } while (0)

    syscall(SYS_delete_module, "hello_user", 0);
    delete_test_module(); /* clean up a prior interrupted run */

    char modules[1024];
    MODULE_CHECK(read_file("/proc/modules", modules, sizeof(modules)) >= 0);
    MODULE_CHECK(strstr(modules, "virtio_net ") != NULL);
    MODULE_CHECK(syscall(SYS_delete_module, "virtio_net", 0) == 0);
    MODULE_CHECK(read_file("/proc/modules", modules, sizeof(modules)) >= 0);
    MODULE_CHECK(strstr(modules, "virtio_net ") == NULL);
    MODULE_CHECK(load_module_file("/lib/modules/virtio_net.ko") == 0);
    MODULE_CHECK(read_file("/proc/modules", modules, sizeof(modules)) >= 0);
    MODULE_CHECK(strstr(modules, "virtio_net ") != NULL);

    fd = open(path, O_RDONLY);
    MODULE_CHECK(fd >= 0);
    MODULE_CHECK(syscall(SYS_finit_module, fd, "", 1) == -1);
    MODULE_CHECK(errno == EINVAL);
    MODULE_CHECK(syscall(SYS_finit_module, fd, "", 0) == 0);
    loaded = 1;

    MODULE_CHECK(read_file("/proc/modules", modules, sizeof(modules)) >= 0);
    MODULE_CHECK(strstr(modules, "hello ") != NULL);
    MODULE_CHECK(syscall(SYS_finit_module, fd, "", 0) == -1);
    MODULE_CHECK(errno == EEXIST);
    close(fd);
    fd = -1;

    MODULE_CHECK(load_module_file("/lib/modules/hello_user.ko") == 0);
    dependent_loaded = 1;
    MODULE_CHECK(delete_test_module() == -1);
    MODULE_CHECK(errno == EBUSY);
    MODULE_CHECK(read_file("/proc/modules", modules, sizeof(modules)) >= 0);
    MODULE_CHECK(strstr(modules, "hello_user ") != NULL);
    MODULE_CHECK(syscall(SYS_delete_module, "hello_user", 0) == 0);
    dependent_loaded = 0;
    MODULE_CHECK(delete_test_module() == 0);
    loaded = 0;
    MODULE_CHECK(delete_test_module() == -1);
    MODULE_CHECK(errno == ENOENT);

    fd = open(path, O_RDONLY);
    struct stat st;
    MODULE_CHECK(fd >= 0);
    MODULE_CHECK(fstat(fd, &st) == 0 && st.st_size > 0);
    image = malloc((size_t) st.st_size);
    MODULE_CHECK(image != NULL);
    MODULE_CHECK(read(fd, image, (size_t) st.st_size) == st.st_size);
    close(fd);
    fd = -1;
    MODULE_CHECK(syscall(SYS_init_module, image, (size_t) st.st_size, "") == 0);
    loaded = 1;
    MODULE_CHECK(delete_test_module() == 0);
    loaded = 0;
    free(image);
    image = NULL;

    unsigned char invalid[16] = {0};
    MODULE_CHECK(syscall(SYS_init_module, invalid, sizeof(invalid), "") == -1);
    MODULE_CHECK(errno == ENOEXEC);

cleanup:
    if (fd >= 0) close(fd);
    free(image);
    if (dependent_loaded) syscall(SYS_delete_module, "hello_user", 0);
    if (loaded) delete_test_module();
    return ok ? TEST_PASS : TEST_FAIL;
#undef MODULE_CHECK
}
REGISTER_TEST(kernel_modules, "Phase 8: Random / Misc");
