#include "test_harness.h"
#include <stdbool.h>
#include <sys/inotify.h>

static int read_events(int fd, char *buf, size_t size)
{
    ssize_t n = read(fd, buf, size);

    if (n < 0) {
        return -1;
    }

    return (int) n;
}

static int has_event(char *buf, int len, uint32_t mask, const char *name, int *wd)
{
    int off = 0;

    while (off + (int) sizeof(struct inotify_event) <= len) {
        struct inotify_event *ev = (struct inotify_event *) (buf + off);
        bool match = (ev->mask & mask) == mask;

        if (match && name) {
            match = ev->len > 0 && strcmp(ev->name, name) == 0;
        }

        if (match) {
            if (wd) {
                *wd = ev->wd;
            }

            return 1;
        }

        off += (int) sizeof(*ev) + (int) ev->len;
    }

    return 0;
}

int test_inotify_init_flags(void)
{
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

    ASSERT_GE(fd, 0);
    close(fd);
    fd = inotify_init1(0x10000000);

    ASSERT_EQ(-1, fd);
    ASSERT_ERRNO(EINVAL);
    return 1;
}
REGISTER_TEST(inotify_init_flags, "Phase 11: inotify");

int test_inotify_create_poll(void)
{
    char path[PATH_MAX];
    char buf[4096];
    int fd = inotify_init1(IN_NONBLOCK);

    ASSERT_GE(fd, 0);
    int wd = inotify_add_watch(fd, tmpdir, IN_CREATE);

    ASSERT_GE(wd, 0);
    tmpfile_path(path, sizeof(path), "created");
    ASSERT_TRUE(write_file(path, "data"));
    struct pollfd pfd = { fd, POLLIN, 0 };

    ASSERT_EQ(1, poll(&pfd, 1, 1000));
    ASSERT_TRUE(pfd.revents & POLLIN);
    int n = read_events(fd, buf, sizeof(buf));

    ASSERT_GT(n, 0);
    ASSERT_TRUE(has_event(buf, n, IN_CREATE, "created", NULL));
    unlink(path);
    close(fd);
    return 1;
}
REGISTER_TEST(inotify_create_poll, "Phase 11: inotify");

int test_inotify_modify_attrib(void)
{
    char path[PATH_MAX];
    char buf[4096];
    tmpfile_path(path, sizeof(path), "watched");
    ASSERT_TRUE(write_file(path, "one"));
    int fd = inotify_init1(IN_NONBLOCK);

    ASSERT_GE(fd, 0);
    ASSERT_GE(inotify_add_watch(fd, path, IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE), 0);
    int file = open(path, O_WRONLY | O_TRUNC);

    ASSERT_GE(file, 0);
    ASSERT_EQ(4, write(file, "data", 4));
    close(file);
    int n = read_events(fd, buf, sizeof(buf));

    ASSERT_GT(n, 0);
    ASSERT_TRUE(has_event(buf, n, IN_MODIFY, NULL, NULL));
    ASSERT_TRUE(has_event(buf, n, IN_CLOSE_WRITE, NULL, NULL));
    ASSERT_EQ(0, chmod(path, 0600));
    n = read_events(fd, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    ASSERT_TRUE(has_event(buf, n, IN_ATTRIB, NULL, NULL));
    unlink(path);
    close(fd);
    return 1;
}
REGISTER_TEST(inotify_modify_attrib, "Phase 11: inotify");

int test_inotify_rename(void)
{
    char oldpath[PATH_MAX], newpath[PATH_MAX], buf[4096];
    tmpfile_path(oldpath, sizeof(oldpath), "oldname");
    tmpfile_path(newpath, sizeof(newpath), "newname");
    ASSERT_TRUE(write_file(oldpath, "data"));
    int fd = inotify_init1(IN_NONBLOCK);

    ASSERT_GE(fd, 0);
    ASSERT_GE(inotify_add_watch(fd, tmpdir, IN_MOVED_FROM | IN_MOVED_TO), 0);
    char drain[4096];
    read_events(fd, drain, sizeof(drain));
    ASSERT_EQ(0, rename(oldpath, newpath));
    int n = read_events(fd, buf, sizeof(buf));

    ASSERT_GT(n, 0);
    int off = 0;
    int from = 0;
    int to = 0;
    uint32_t from_cookie = 0;
    uint32_t to_cookie = 0;

    while (off + (int) sizeof(struct inotify_event) <= n) {
        struct inotify_event *ev = (struct inotify_event *) (buf + off);

        if (ev->mask & IN_MOVED_FROM) {
            from = ev->len > 0 && strcmp(ev->name, "oldname") == 0;
            from_cookie = ev->cookie;
        }

        if (ev->mask & IN_MOVED_TO) {
            to = ev->len > 0 && strcmp(ev->name, "newname") == 0;
            to_cookie = ev->cookie;
        }

        off += (int) sizeof(*ev) + (int) ev->len;
    }

    ASSERT_TRUE(from);
    ASSERT_TRUE(to);
    ASSERT_NE(0, from_cookie);
    ASSERT_EQ(from_cookie, to_cookie);
    unlink(newpath);
    close(fd);
    return 1;
}
REGISTER_TEST(inotify_rename, "Phase 11: inotify");

int test_inotify_delete_self(void)
{
    char path[PATH_MAX], buf[4096];
    tmpfile_path(path, sizeof(path), "deleted");
    ASSERT_TRUE(write_file(path, "data"));
    int fd = inotify_init1(IN_NONBLOCK);

    ASSERT_GE(fd, 0);
    ASSERT_GE(inotify_add_watch(fd, path, IN_DELETE_SELF | IN_IGNORED), 0);
    ASSERT_EQ(0, unlink(path));
    int n = read_events(fd, buf, sizeof(buf));

    ASSERT_GT(n, 0);
    ASSERT_TRUE(has_event(buf, n, IN_DELETE_SELF, NULL, NULL));
    ASSERT_TRUE(has_event(buf, n, IN_IGNORED, NULL, NULL));
    close(fd);
    return 1;
}
REGISTER_TEST(inotify_delete_self, "Phase 11: inotify");

int test_inotify_watch_flags(void)
{
    char path[PATH_MAX], buf[4096];
    tmpfile_path(path, sizeof(path), "plain");
    ASSERT_TRUE(write_file(path, "data"));
    int fd = inotify_init1(IN_NONBLOCK);

    ASSERT_GE(fd, 0);
    ASSERT_EQ(-1, inotify_add_watch(fd, path, IN_ONLYDIR));
    ASSERT_ERRNO(ENOTDIR);
    int wd = inotify_add_watch(fd, tmpdir, IN_ATTRIB);

    ASSERT_GE(wd, 0);
    ASSERT_EQ(0, inotify_rm_watch(fd, wd));
    int n = read_events(fd, buf, sizeof(buf));

    ASSERT_GT(n, 0);
    ASSERT_TRUE(has_event(buf, n, IN_IGNORED, NULL, NULL));
    ASSERT_EQ(-1, inotify_rm_watch(fd, wd));
    ASSERT_ERRNO(EINVAL);
    unlink(path);
    close(fd);
    return 1;
}
REGISTER_TEST(inotify_watch_flags, "Phase 11: inotify");

int test_inotify_oneshot(void)
{
    char path[PATH_MAX], buf[4096];
    int fd = inotify_init1(IN_NONBLOCK);

    ASSERT_GE(fd, 0);
    ASSERT_GE(inotify_add_watch(fd, tmpdir, IN_CREATE | IN_ONESHOT), 0);
    tmpfile_path(path, sizeof(path), "first");
    ASSERT_TRUE(write_file(path, "data"));
    int n = read_events(fd, buf, sizeof(buf));

    ASSERT_GT(n, 0);
    ASSERT_TRUE(has_event(buf, n, IN_CREATE, "first", NULL));
    tmpfile_path(path, sizeof(path), "second");
    ASSERT_TRUE(write_file(path, "data"));
    n = read_events(fd, buf, sizeof(buf));

    ASSERT_EQ(-1, n);
    ASSERT_ERRNO(EAGAIN);
    unlink(path);
    close(fd);
    return 1;
}
REGISTER_TEST(inotify_oneshot, "Phase 11: inotify");

int test_inotify_mask_add(void)
{
    char path[PATH_MAX], buf[4096];
    int fd = inotify_init1(IN_NONBLOCK);

    ASSERT_GE(fd, 0);
    int wd = inotify_add_watch(fd, tmpdir, IN_CREATE);

    ASSERT_GE(wd, 0);
    ASSERT_EQ(wd, inotify_add_watch(fd, tmpdir, IN_DELETE | IN_MASK_ADD));
    tmpfile_path(path, sizeof(path), "mask");
    ASSERT_TRUE(write_file(path, "data"));
    ASSERT_EQ(0, unlink(path));
    int n = read_events(fd, buf, sizeof(buf));

    ASSERT_GT(n, 0);
    ASSERT_TRUE(has_event(buf, n, IN_CREATE, "mask", NULL));
    ASSERT_TRUE(has_event(buf, n, IN_DELETE, "mask", NULL));
    close(fd);
    return 1;
}
REGISTER_TEST(inotify_mask_add, "Phase 11: inotify");
