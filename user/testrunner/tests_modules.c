#include "test_harness.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

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
    if (strstr(modules, "virtio_net ") != NULL) {
        MODULE_CHECK(syscall(SYS_delete_module, "virtio_net", 0) == 0);
        MODULE_CHECK(read_file("/proc/modules", modules, sizeof(modules)) >= 0);
        MODULE_CHECK(strstr(modules, "virtio_net ") == NULL);
        MODULE_CHECK(load_module_file("/lib/modules/virtio_net.ko") == 0);
        MODULE_CHECK(read_file("/proc/modules", modules, sizeof(modules)) >= 0);
        MODULE_CHECK(strstr(modules, "virtio_net ") != NULL);
    }
    if (strstr(modules, "e1000 ") != NULL) {
        MODULE_CHECK(syscall(SYS_delete_module, "e1000", 0) == 0);
        MODULE_CHECK(read_file("/proc/modules", modules, sizeof(modules)) >= 0);
        MODULE_CHECK(strstr(modules, "e1000 ") == NULL);
        MODULE_CHECK(load_module_file("/lib/modules/e1000.ko") == 0);
        MODULE_CHECK(read_file("/proc/modules", modules, sizeof(modules)) >= 0);
        MODULE_CHECK(strstr(modules, "e1000 ") != NULL);
    }

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

int test_e1000_net_transmit(void) {
    char modules[1024];
    if (read_file("/proc/modules", modules, sizeof(modules)) < 0) return TEST_FAIL;
    if (strstr(modules, "e1000 ") == NULL) return TEST_PASS;

    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) return TEST_FAIL;

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = inet_addr("10.0.2.2");

    uint8_t pkt[sizeof(struct icmphdr) + 32];
    struct icmphdr *icmp = (struct icmphdr *) pkt;
    memset(icmp, 0, sizeof(*icmp));
    icmp->type = ICMP_ECHO;
    icmp->code = 0;
    icmp->un.echo.id = htons((uint16_t) getpid());
    icmp->un.echo.sequence = htons(1);

    uint8_t *payload = pkt + sizeof(struct icmphdr);
    for (int i = 0; i < 32; i++) payload[i] = (uint8_t) i;

    const uint16_t *p = (const uint16_t *) pkt;
    int len = (int) sizeof(pkt);
    uint32_t sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len) sum += *(const uint8_t *) p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    icmp->checksum = (uint16_t) ~sum;

    ssize_t sent = sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *) &to, sizeof(to));
    if (sent != (ssize_t) sizeof(pkt)) {
        close(fd);
        return TEST_FAIL;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, 1000);
    if (r > 0) {
        uint8_t rbuf[512];
        recv(fd, rbuf, sizeof(rbuf), 0);
    }
    close(fd);
    return TEST_PASS;
}
REGISTER_TEST(e1000_net_transmit, "Phase 8: Random / Misc");
