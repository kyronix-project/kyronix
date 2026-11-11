#include "common.h"
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/wait.h>

#ifndef LOOP_SET_FD
#define LOOP_SET_FD 0x4C00
#define LOOP_CLR_FD 0x4C01
#define LOOP_CONFIGURE 0x4C0A
#endif

#define LO_NAME_SIZE 64
#define LO_KEY_SIZE 32
#define LOOP_DEV_MAX 8
#define SCAN_STEP 512

typedef struct {
    uint64_t lo_device, lo_inode, lo_rdevice, lo_offset, lo_sizelimit;
    uint32_t lo_number, lo_encrypt_type, lo_encrypt_key_size, lo_flags;
    uint8_t lo_file_name[LO_NAME_SIZE];
    uint8_t lo_crypt_name[LO_NAME_SIZE];
    uint8_t lo_encrypt_key[LO_KEY_SIZE];
    uint64_t lo_init[2];
} loop_info64_t;

typedef struct {
    int32_t fd;
    uint32_t block_size;
    loop_info64_t info;
    uint64_t reserved[8];
} loop_config_t;

extern char *realpath(const char *restrict path, char *restrict resolved);

static void usage(void) {
    fprintf(stderr, "usage: %s [options] <AppImage> [args...]\n", kx_prog);
    fprintf(stderr, "\noptions:\n");
    fprintf(stderr, "  --appimage-version         print version and exit\n");
    fprintf(stderr, "  --appimage-extract-and-run  accepted for compatibility (default behaviour)\n");
}

static int open_image(const char *path, char *abs) {
    if (!realpath(path, abs)) {
        kx_warn(path);
        return -1;
    }

    int fd = open(abs, O_RDONLY);
    if (fd < 0) {
        kx_warn(abs);
        return -1;
    }

    unsigned char head[512];
    ssize_t n = read(fd, head, sizeof(head));
    if (n < 12) {
        fprintf(stderr, "%s: %s: file too short\n", kx_prog, abs);
        close(fd);
        return -1;
    }
    if (memcmp(head, "\x7f" "ELF", 4) != 0) {
        fprintf(stderr, "%s: %s: not an AppImage (missing ELF magic)\n", kx_prog, abs);
        close(fd);
        return -1;
    }
    if (!(head[8] == 0x41 && head[9] == 0x49 && head[10] == 0x02)) {
        fprintf(stderr, "%s: %s: not a type-2 AppImage (missing 0x414902 magic at offset 8)\n",
                kx_prog, abs);
        close(fd);
        return -1;
    }
    return fd;
}

static int64_t find_sqfs_offset(int fd, off_t size) {
    unsigned char buf[4];

    for (int phase = 0; phase < 2; phase++) {
        int step = phase == 0 ? 4096 : SCAN_STEP;
        for (off_t off = step; off + 4 <= size; off += step) {
            if (pread(fd, buf, 4, off) != 4) break;
            if (buf[0] == 'h' && buf[1] == 's' && buf[2] == 'q' && buf[3] == 's') return (int64_t) off;
        }
    }
    return -1;
}

static int loop_acquire(int img_fd, int64_t sqfs_off, char *dev, size_t devsz) {
    for (int i = 0; i < LOOP_DEV_MAX; i++) {
        snprintf(dev, devsz, "/dev/loop%d", i);
        int lfd = open(dev, O_RDONLY);
        if (lfd < 0) continue;

        loop_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.fd = img_fd;
        cfg.info.lo_offset = (uint64_t) sqfs_off;

        if (ioctl(lfd, LOOP_CONFIGURE, &cfg) == 0) return lfd;
        if (errno != EBUSY) {
            kx_warn(dev);
            close(lfd);
            return -1;
        }
        close(lfd);
    }
    fprintf(stderr, "%s: no free loop device\n", kx_prog);
    return -1;
}

static char **build_env(const char *abs, const char *mp, char *e_image, size_t e_image_sz,
                        char *e_dir, size_t e_dir_sz) {
    extern char **environ;
    int count = 0;
    while (environ[count]) count++;

    char **env = calloc((size_t) count + 4, sizeof(char *));
    if (!env) return NULL;

    snprintf(e_image, e_image_sz, "APPIMAGE=%s", abs);
    snprintf(e_dir, e_dir_sz, "APPDIR=%s", mp);

    int n = 0;
    for (int i = 0; i < count; i++) {
        if (strncmp(environ[i], "APPIMAGE=", 9) == 0 || strncmp(environ[i], "APPDIR=", 7) == 0 ||
            strncmp(environ[i], "APPIMAGE_EXTRACT_AND_RUN=", 25) == 0)
            continue;
        env[n++] = environ[i];
    }
    env[n++] = e_image;
    env[n++] = e_dir;
    env[n++] = "APPIMAGE_EXTRACT_AND_RUN=1";
    env[n] = NULL;
    return env;
}

int main(int argc, char **argv) {
    kx_prog = kx_base(argv[0]);

    int ai = 1;
    while (ai < argc && strncmp(argv[ai], "--appimage-", 11) == 0) {
        if (strcmp(argv[ai], "--appimage-version") == 0) {
            printf("appimage-run 1.0 (kyronix)\n");
            return 0;
        }
        else if (strcmp(argv[ai], "--appimage-help") == 0) {
            usage();
            return 0;
        }
        else if (strcmp(argv[ai], "--appimage-extract-and-run") == 0) {
            ai++;
        }
        else {
            fprintf(stderr, "%s: unknown option: %s\n", kx_prog, argv[ai]);
            usage();
            return 1;
        }
    }

    if (argc - ai < 1) {
        usage();
        return 1;
    }
    const char *image = argv[ai];
    int n_args = argc - ai - 1;
    char **args = argv + ai + 1;

    char abs[PATH_MAX];
    int img_fd = open_image(image, abs);
    if (img_fd < 0) return 1;

    struct stat st;
    if (fstat(img_fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 4096) {
        fprintf(stderr, "%s: %s: not an AppImage\n", kx_prog, image);
        close(img_fd);
        return 1;
    }

    int64_t sqfs_off = find_sqfs_offset(img_fd, st.st_size);
    if (sqfs_off < 0) {
        fprintf(stderr, "%s: %s: no squashfs image found\n", kx_prog, image);
        close(img_fd);
        return 1;
    }

    char dev[32];
    int lfd = loop_acquire(img_fd, sqfs_off, dev, sizeof(dev));
    if (lfd < 0) {
        close(img_fd);
        return 1;
    }

    char mp[] = "/tmp/aimnt-XXXXXX";
    if (!mkdtemp(mp)) {
        kx_warn("/tmp");
        close(lfd);
        close(img_fd);
        return 1;
    }

    if (mount(dev, mp, "squashfs", MS_RDONLY, NULL) < 0) {
        fprintf(stderr, "%s: %s on %s: %s\n", kx_prog, dev, mp, strerror(errno));
        rmdir(mp);
        close(lfd);
        close(img_fd);
        return 1;
    }

    char apprun[PATH_MAX];
    snprintf(apprun, sizeof(apprun), "%s/AppRun", mp);
    if (stat(apprun, &st) < 0 || !S_ISREG(st.st_mode) || !(st.st_mode & 0111)) {
        fprintf(stderr, "%s: %s: AppRun not found or not executable\n", kx_prog, image);
        umount2(mp, 0);
        rmdir(mp);
        close(lfd);
        close(img_fd);
        return 1;
    }

    char e_image[PATH_MAX + 16];
    char e_dir[PATH_MAX + 16];
    char **env = build_env(abs, mp, e_image, sizeof(e_image), e_dir, sizeof(e_dir));
    if (!env) {
        fprintf(stderr, "%s: out of memory\n", kx_prog);
        umount2(mp, 0);
        rmdir(mp);
        close(lfd);
        close(img_fd);
        return 1;
    }

    char **cargv = calloc((size_t) n_args + 2, sizeof(char *));
    if (!cargv) {
        fprintf(stderr, "%s: out of memory\n", kx_prog);
        umount2(mp, 0);
        rmdir(mp);
        close(lfd);
        close(img_fd);
        return 1;
    }
    cargv[0] = apprun;
    for (int i = 0; i < n_args; i++) cargv[i + 1] = args[i];
    cargv[n_args + 1] = NULL;

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        kx_warn("fork");
        umount2(mp, 0);
        rmdir(mp);
        close(lfd);
        close(img_fd);
        return 1;
    }
    if (pid == 0) {
        chdir(mp);
        execve(apprun, cargv, env);
        fprintf(stderr, "%s: %s: %s\n", kx_prog, apprun, strerror(errno));
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    umount2(mp, 0);
    ioctl(lfd, LOOP_CLR_FD);
    rmdir(mp);
    close(lfd);
    close(img_fd);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
