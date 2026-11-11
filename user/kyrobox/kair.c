#define _DEFAULT_SOURCE
#include "common.h"
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/wait.h>

#ifndef CLONE_NEWNS
#define CLONE_NEWNS 0x00020000
#endif
extern int unshare(int flags);

#define SYSROOT_IMG "/usr/lib/sysroots/debian.sqsh"

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

static int sb_bind(const char *src, const char *dst) {
    if (mount(src, dst, NULL, MS_BIND | MS_RDONLY, NULL) < 0) return -1;
    return 0;
}

static int sb_mkdir(char *base, const char *rel) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", base, rel);
    return kx_mkdir_p(path, 0755);
}

static int apprun_self_contained(const char *apprun_path, const char *mp) {
    (void) apprun_path;
    char libs[PATH_MAX];
    snprintf(libs, sizeof(libs), "%s/libs", mp);
    struct stat st;
    return stat(libs, &st) == 0 && S_ISDIR(st.st_mode);
}

static int apprun_is_elf(const char *apprun_path) {
    int fd = open(apprun_path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char m[4];
    ssize_t n = read(fd, m, sizeof(m));
    close(fd);
    return n == 4 && memcmp(m, "\x7f" "ELF", 4) == 0;
}

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

static char **build_env(const char *abs, const char *mp) {
    extern char **environ;
    int count = 0;
    while (environ[count]) count++;

    char **env = calloc((size_t) count + 4, sizeof(char *));
    if (!env) return NULL;

    size_t need = strlen(abs) + strlen(mp) + 32;
    char *e_image = malloc(need);
    char *e_dir = malloc(need);
    if (!e_image || !e_dir) { free(env); free(e_image); free(e_dir); return NULL; }
    snprintf(e_image, need, "APPIMAGE=%s", abs);
    snprintf(e_dir, need, "APPDIR=%s", mp);

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

static void free_env(char **env) {
    if (!env) return;

    extern char **environ;
    for (int i = 0; env[i]; i++) {
        if (strncmp(env[i], "APPIMAGE=", 9) == 0 || strncmp(env[i], "APPDIR=", 7) == 0) {
            int inherited = 0;
            for (int j = 0; environ[j]; j++) if (environ[j] == env[i]) { inherited = 1; break; }
            if (!inherited) free(env[i]);
        }
    }
    free(env);
}

int main(int argc, char **argv) {
    kx_prog = kx_base(argv[0]);

    int ai = 1;
    while (ai < argc && strncmp(argv[ai], "--appimage-", 11) == 0) {
        if (strcmp(argv[ai], "--appimage-version") == 0) {
            printf("kair 1.0 (kyronix)\n");
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

    int use_sandbox = apprun_is_elf(apprun);
    int self_contained = apprun_self_contained(apprun, mp);

    char sb[] = "/tmp/aisb-XXXXXX";
    char sb_app[PATH_MAX] = {0}, sb_proc[PATH_MAX] = {0};
    char sr_mp[] = "/tmp/aisr-XXXXXX";
    int sb_dir_exists = 0, sb_ready = 0, tmpfs_mounted = 0, app_bound = 0, proc_bound = 0;
    int sr_created = 0, sr_mounted = 0;
    int sr_loopfd = -1;

    char *sb_bound_paths[16];
    int n_sb_bound = 0;

    if (use_sandbox && mkdtemp(sb)) {
        sb_dir_exists = 1;
        do {
            if (mount("tmpfs", sb, "tmpfs", 0, "mode=0755") < 0) break;
            tmpfs_mounted = 1;
            snprintf(sb_app, sizeof(sb_app), "%s/app", sb);
            if (mkdir(sb_app, 0755) < 0) break;
            if (sb_bind(mp, sb_app) < 0) break;
            app_bound = 1;
            snprintf(sb_proc, sizeof(sb_proc), "%s/proc", sb);
            if (mkdir(sb_proc, 0755) < 0) break;
            if (sb_bind("/proc", sb_proc) < 0) break;
            proc_bound = 1;

            const char *hostdirs[] = {"bin", "dev"};
            for (unsigned hi = 0; hi < sizeof(hostdirs)/sizeof(hostdirs[0]); hi++) {
                char src[PATH_MAX], dst[PATH_MAX];
                struct stat tst;
                snprintf(src, sizeof(src), "/%s", hostdirs[hi]);
                if (stat(src, &tst) != 0 || !S_ISDIR(tst.st_mode)) continue;
                if (sb_mkdir(sb, hostdirs[hi]) < 0) break;
                snprintf(dst, sizeof(dst), "%s/%s", sb, hostdirs[hi]);
                if (sb_bind(src, dst) < 0) break;
                sb_bound_paths[n_sb_bound++] = strdup(dst);
            }

            if (self_contained) {
                char src[PATH_MAX], dst[PATH_MAX];
                snprintf(src, sizeof(src), "%s/libs", mp);
                struct stat bst;
                if (stat(src, &bst) != 0 || !S_ISDIR(bst.st_mode)) break;
                // /lib64 holds the interpreter referenced by PT_INTERP
                if (sb_mkdir(sb, "lib64") < 0) break;
                snprintf(dst, sizeof(dst), "%s/lib64", sb);
                if (sb_bind(src, dst) < 0) break;
                sb_bound_paths[n_sb_bound++] = strdup(dst);
                // standard multiarch dir for absolute libc lookups 
                if (sb_mkdir(sb, "lib/x86_64-linux-gnu") < 0) break;
                snprintf(dst, sizeof(dst), "%s/lib/x86_64-linux-gnu", sb);
                if (sb_bind(src, dst) < 0) break;
                sb_bound_paths[n_sb_bound++] = strdup(dst);
            } else {
                struct stat sst;
                if (stat(SYSROOT_IMG, &sst) == 0 && S_ISREG(sst.st_mode)) {
                    if (!mkdtemp(sr_mp)) break;
                    sr_created = 1;
                    int sfd = open(SYSROOT_IMG, O_RDONLY);
                    if (sfd < 0) break;
                    char sdev[32];
                    sr_loopfd = loop_acquire(sfd, 0, sdev, sizeof(sdev));
                    close(sfd);
                    if (sr_loopfd < 0) break;
                    if (mount(sdev, sr_mp, "squashfs", MS_RDONLY, NULL) < 0) break;
                    sr_mounted = 1;
                    const char *dirs[] = {"lib", "lib64", "usr"};
                    int ok = 1;
                    for (unsigned di = 0; di < sizeof(dirs)/sizeof(dirs[0]); di++) {
                        char src[PATH_MAX], dst[PATH_MAX];
                        struct stat tst;
                        snprintf(src, sizeof(src), "%s/%s", sr_mp, dirs[di]);
                        if (stat(src, &tst) != 0 || !S_ISDIR(tst.st_mode)) continue;
                        if (sb_mkdir(sb, dirs[di]) < 0) { ok = 0; break; }
                        snprintf(dst, sizeof(dst), "%s/%s", sb, dirs[di]);
                        if (sb_bind(src, dst) < 0) { ok = 0; break; }
                        sb_bound_paths[n_sb_bound++] = strdup(dst);
                    }
                    const char *etcfiles[] = {"nsswitch.conf", "passwd", "group", "localtime"};
                    for (unsigned ei = 0; ei < sizeof(etcfiles)/sizeof(etcfiles[0]); ei++) {
                        char src[PATH_MAX], dst[PATH_MAX];
                        struct stat etst;
                        snprintf(src, sizeof(src), "%s/etc/%s", sr_mp, etcfiles[ei]);
                        if (stat(src, &etst) != 0) continue;
                        if (sb_mkdir(sb, "etc") < 0) { ok = 0; break; }
                        snprintf(dst, sizeof(dst), "%s/etc/%s", sb, etcfiles[ei]);
                        int tf = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if (tf >= 0) close(tf);
                        if (sb_bind(src, dst) < 0) { ok = 0; break; }
                        sb_bound_paths[n_sb_bound++] = strdup(dst);
                    }
                    if (!ok) break;
                }
            }
            sb_ready = 1; // all mandatory mounts succeeded
        } while (0);
    }

    char apprun_final[PATH_MAX];
    const char *exec_apprun;
    const char *workdir;
    char **env;

    if (sb_ready) {
        snprintf(apprun_final, sizeof(apprun_final), "/app/AppRun");
        exec_apprun = apprun_final;
        workdir = sb;
        char ad_in_sb[PATH_MAX];
        snprintf(ad_in_sb, sizeof(ad_in_sb), "%s/app", sb);
        env = build_env(abs, ad_in_sb);
    } else {
        exec_apprun = apprun;
        workdir = mp;
        env = build_env(abs, mp);
    }
    if (!env) {
        fprintf(stderr, "%s: out of memory\n", kx_prog);
        goto cleanup;
    }

    char **cargv = calloc((size_t) n_args + 2, sizeof(char *));
    if (!cargv) {
        fprintf(stderr, "%s: out of memory\n", kx_prog);
        free_env(env);
        goto cleanup;
    }
    cargv[0] = (char *) exec_apprun;
    for (int i = 0; i < n_args; i++) cargv[i + 1] = args[i];
    cargv[n_args + 1] = NULL;

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        kx_warn("fork");
        free(cargv);
        free_env(env);
        goto cleanup;
    }
    if (pid == 0) {
        if (sb_ready) {
            unshare(CLONE_NEWNS);
            if (chroot(sb) < 0) {

                chdir(workdir);
                execve(exec_apprun, cargv, env);
                _exit(127);
            }
            if (chdir("/") < 0) { /* ignore */ }
        } else {
            chdir(workdir);
        }
        execve(exec_apprun, cargv, env);
        fprintf(stderr, "%s: %s: %s\n", kx_prog, exec_apprun, strerror(errno));
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    free(cargv);
    free_env(env);

cleanup:
    // Tear down inner-most mounts first, then outward.
    for (int bi = n_sb_bound - 1; bi >= 0; bi--) {
        if (sb_bound_paths[bi]) { umount2(sb_bound_paths[bi], 0); free(sb_bound_paths[bi]); }
    }
    if (sr_mounted) umount2(sr_mp, 0);
    if (proc_bound) umount2(sb_proc, 0);
    if (app_bound) umount2(sb_app, 0);
    if (tmpfs_mounted) umount2(sb, 0);
    if (sb_dir_exists) rmdir(sb);
    if (sr_loopfd >= 0) {
        ioctl(sr_loopfd, LOOP_CLR_FD);
        close(sr_loopfd);
    }
    if (sr_created) rmdir(sr_mp);
    umount2(mp, 0);
    ioctl(lfd, LOOP_CLR_FD);
    rmdir(mp);
    close(lfd);
    close(img_fd);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
