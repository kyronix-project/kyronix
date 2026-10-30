#include "test_harness.h"
#include <sys/prctl.h>
#include <sys/capability.h>
#include <linux/seccomp.h>
#include <linux/filter.h>

int test_security_prctl_name(void) {
    char orig[16];
    ASSERT_EQ(0, prctl(PR_GET_NAME, orig));
    ASSERT_EQ(0, prctl(PR_SET_NAME, "testproc"));
    char name[16];
    ASSERT_EQ(0, prctl(PR_GET_NAME, name));
    ASSERT_EQ(0, strcmp(name, "testproc"));
    ASSERT_EQ(0, prctl(PR_SET_NAME, orig));
    return 1;
}
REGISTER_TEST(security_prctl_name, "Phase 10: Security ABI");

int test_security_no_new_privs(void) {
    long nnp = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    ASSERT_GE(nnp, 0);
    if (nnp == 0) {
        ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
        ASSERT_EQ(1, prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0));
    }
    return 1;
}
REGISTER_TEST(security_no_new_privs, "Phase 10: Security ABI");

int test_security_dumpable(void) {
    int d = prctl(PR_GET_DUMPABLE);
    ASSERT_GE(d, 0);
    ASSERT_EQ(0, prctl(PR_SET_DUMPABLE, 0));
    ASSERT_EQ(0, prctl(PR_GET_DUMPABLE));
    ASSERT_EQ(0, prctl(PR_SET_DUMPABLE, 1));
    ASSERT_EQ(1, prctl(PR_GET_DUMPABLE));
    return 1;
}
REGISTER_TEST(security_dumpable, "Phase 10: Security ABI");

int test_security_caps(void) {
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[2];
    memset(&hdr, 0, sizeof(hdr));
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    hdr.pid = 0;

    ASSERT_EQ(0, capget(&hdr, data));
    // Try setting permitted/effective (dropping or retaining)
    ASSERT_EQ(0, capset(&hdr, data));
    return 1;
}
REGISTER_TEST(security_caps, "Phase 10: Security ABI");

int test_security_seccomp_mode(void) {
    int mode = prctl(PR_GET_SECCOMP);
    ASSERT_GE(mode, 0);
    return 1;
}
REGISTER_TEST(security_seccomp_mode, "Phase 10: Security ABI");

int test_security_proc_status(void) {
    int fd = open("/proc/self/status", O_RDONLY);
    ASSERT_GE(fd, 0);
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    ASSERT_GT(n, 0);
    buf[n] = '\0';

    ASSERT_TRUE(strstr(buf, "NoNewPrivs:") != NULL);
    ASSERT_TRUE(strstr(buf, "Seccomp:") != NULL);
    ASSERT_TRUE(strstr(buf, "CapEff:") != NULL);
    ASSERT_TRUE(strstr(buf, "CapPrm:") != NULL);
    ASSERT_TRUE(strstr(buf, "CapInh:") != NULL);
    return 1;
}
REGISTER_TEST(security_proc_status, "Phase 10: Security ABI");
