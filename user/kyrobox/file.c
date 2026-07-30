#include "common.h"

static bool flag_L = false;
static bool flag_s = false;
static bool flag_b = false;
static bool flag_i = false;
static bool flag_0 = false;
static const char *flag_F = ": ";

static const char *ftype_name(mode_t m) {
    if (S_ISREG(m)) return "regular file";
    if (S_ISDIR(m)) return "directory";
    if (S_ISLNK(m)) return "symbolic link";
    if (S_ISCHR(m)) return "character special";
    if (S_ISBLK(m)) return "block special";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISSOCK(m)) return "socket";
    return "unknown";
}

static const char *ftype_mime(mode_t m) {
    if (S_ISREG(m)) return "application/octet-stream";
    if (S_ISDIR(m)) return "inode/directory";
    if (S_ISLNK(m)) return "inode/symlink";
    if (S_ISCHR(m)) return "inode/chardevice";
    if (S_ISBLK(m)) return "inode/blockdevice";
    if (S_ISFIFO(m)) return "inode/fifo";
    if (S_ISSOCK(m)) return "inode/socket";
    return "application/octet-stream";
}

static const char *magic_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    unsigned char buf[512];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == 0) return "empty";

    if (n >= 4 && buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F')
        return "ELF";

    if (n >= 2 && buf[0] == '#' && buf[1] == '!') {
        char line[512];
        size_t len = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        memcpy(line, buf, len);
        line[len] = 0;
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *interp = line + 2;
        while (*interp == ' ') interp++;
        char *arg = strchr(interp, ' ');
        if (arg) *arg = 0;
        const char *name = kx_base(interp);
        if (strcmp(name, "sh") == 0) return "POSIX shell script";
        if (strcmp(name, "bash") == 0) return "Bourne-Again shell script";
        if (strcmp(name, "dash") == 0) return "Debian Almquist shell script";
        if (strcmp(name, "awk") == 0) return "awk script";
        if (strcmp(name, "sed") == 0) return "sed script";
        if (strcmp(name, "perl") == 0) return "Perl script";
        if (strcmp(name, "python") == 0 || strcmp(name, "python3") == 0) return "Python script";
        if (strcmp(name, "lua") == 0) return "Lua script";
        if (strcmp(name, "expect") == 0) return "Expect script";
        if (strcmp(name, "make") == 0 || strcmp(name, "gmake") == 0) return "make script";
        return "script";
    }

    if (n >= 4 && buf[0] == '%' && buf[1] == 'P' && buf[2] == 'D' && buf[3] == 'F')
        return "PDF document";
    if (n >= 2 && buf[0] == 0x1f && buf[1] == 0x8b)
        return "gzip compressed data";
    if (n >= 4 && buf[0] == 0x1f && buf[1] == 0x9d)
        return "compress'd data";
    if (n >= 3 && buf[0] == 0x42 && buf[1] == 0x5a && buf[2] == 0x68)
        return "bzip2 compressed data";
    if (n >= 7 && ((buf[0] == 0xfd && buf[1] == '7' && buf[2] == 'z' && buf[3] == 'X' &&
                    buf[4] == 'Z' && buf[5] == 0x00) ||
                   (buf[0] == '7' && buf[1] == 'z' && buf[2] == 0xbc && buf[3] == 0xaf &&
                    buf[4] == 0x27 && buf[5] == 0x1c)))
        return "7-zip archive data";
    if (n >= 4 && buf[0] == 'M' && buf[1] == 'Z')
        return "DOS/MBR executable";
    if (n >= 257) {
        if (memcmp(buf + 257, "ustar", 5) == 0) {
            if (buf[262] == ' ') return "POSIX tar archive";
            if (buf[262] == 0) return "GNU tar archive";
        }
    }
    if (n >= 4 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G')
        return "PNG image data";
    if (n >= 2 && buf[0] == 0xff && buf[1] == 0xd8)
        return "JPEG image data";

    bool printable = true;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == 0) { printable = false; break; }
    }
    if (printable) return "ASCII text";
    return "data";
}

static const char *magic_mime(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    unsigned char buf[512];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n == 0) return "application/x-empty";

    if (n >= 4 && buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F')
        return "application/x-executable";

    if (n >= 2 && buf[0] == '#' && buf[1] == '!') {
        char line[512];
        size_t len = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        memcpy(line, buf, len);
        line[len] = 0;
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *interp = line + 2;
        while (*interp == ' ') interp++;
        char *arg = strchr(interp, ' ');
        if (arg) *arg = 0;
        const char *name = kx_base(interp);
        if (strcmp(name, "sh") == 0 || strcmp(name, "bash") == 0 || strcmp(name, "dash") == 0)
            return "text/x-shellscript";
        if (strcmp(name, "awk") == 0) return "text/x-awk";
        if (strcmp(name, "sed") == 0) return "text/x-sed";
        if (strcmp(name, "perl") == 0) return "text/x-perl";
        if (strcmp(name, "python") == 0 || strcmp(name, "python3") == 0) return "text/x-python";
        if (strcmp(name, "lua") == 0) return "text/x-lua";
        return "text/plain";
    }

    if (n >= 4 && buf[0] == '%' && buf[1] == 'P' && buf[2] == 'D' && buf[3] == 'F')
        return "application/pdf";
    if (n >= 2 && buf[0] == 0x1f && buf[1] == 0x8b)
        return "application/gzip";
    if (n >= 4 && buf[0] == 0x1f && buf[1] == 0x9d)
        return "application/x-compress";
    if (n >= 3 && buf[0] == 0x42 && buf[1] == 0x5a && buf[2] == 0x68)
        return "application/x-bzip2";
    if (n >= 7 && ((buf[0] == 0xfd && buf[1] == '7' && buf[2] == 'z' && buf[3] == 'X' &&
                    buf[4] == 'Z' && buf[5] == 0x00) ||
                   (buf[0] == '7' && buf[1] == 'z' && buf[2] == 0xbc && buf[3] == 0xaf &&
                    buf[4] == 0x27 && buf[5] == 0x1c)))
        return "application/x-7z-compressed";
    if (n >= 4 && buf[0] == 'M' && buf[1] == 'Z')
        return "application/x-dosexec";
    if (n >= 257) {
        if (memcmp(buf + 257, "ustar", 5) == 0)
            return "application/x-tar";
    }
    if (n >= 4 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G')
        return "image/png";
    if (n >= 2 && buf[0] == 0xff && buf[1] == 0xd8)
        return "image/jpeg";

    bool printable = true;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == 0) { printable = false; break; }
    }
    if (printable) return "text/plain";
    return "application/octet-stream";
}

static void do_file(const char *path) {
    struct stat st;
    int r = flag_L ? stat(path, &st) : lstat(path, &st);
    if (r < 0) {
        if (flag_b) {
            fprintf(stderr, "%s\n", strerror(errno));
        } else {
            kx_warn(path);
        }
        return;
    }

    const char *desc = NULL;
    if (S_ISREG(st.st_mode) && !flag_s) {
        desc = flag_i ? magic_mime(path) : magic_file(path);
    }

    if (!desc)
        desc = flag_i ? ftype_mime(st.st_mode) : ftype_name(st.st_mode);

    if (flag_b) {
        fputs(desc, stdout);
    } else {
        printf("%s%s%s", path, flag_F, desc);
    }

    if (flag_0)
        putchar(0);
    else
        putchar('\n');
}

int main(int argc, char **argv) {
    kx_prog = "file";

    int first = 1;
    for (; first < argc; first++) {
        const char *arg = argv[first];
        if (strcmp(arg, "--") == 0) { first++; break; }
        if (arg[0] != '-' || arg[1] == '\0') break;

        if (strcmp(arg, "--help") == 0) {
            fprintf(stderr, "usage: file [-bhiL0s] [-F SEP] FILE...\n");
            fprintf(stderr, "  -b          brief (no filename prefix)\n");
            fprintf(stderr, "  -F SEP      use SEP as separator (default: \": \")\n");
            fprintf(stderr, "  -h          show help\n");
            fprintf(stderr, "  -i          mime type output\n");
            fprintf(stderr, "  -L          follow symlinks\n");
            fprintf(stderr, "  -0          null-terminate output\n");
            fprintf(stderr, "  -s          stat only (don't read contents)\n");
            return 0;
        }

        if ((strcmp(arg, "-F") == 0) && first + 1 < argc) {
            flag_F = argv[++first];
            continue;
        }

        for (const char *p = arg + 1; *p; p++) {
            switch (*p) {
            case 'b': flag_b = true; break;
            case 'h': fprintf(stderr, "usage: file [-bhiL0s] [-F SEP] FILE...\n"); return 0;
            case 'i': flag_i = true; break;
            case 'L': flag_L = true; break;
            case '0': flag_0 = true; break;
            case 's': flag_s = true; break;
            default:
                fprintf(stderr, "usage: file [-bhiL0s] [-F SEP] FILE...\n");
                return 1;
            }
        }
    }

    if (first == argc) {
        fprintf(stderr, "usage: file [-bhiL0s] [-F SEP] FILE...\n");
        return 1;
    }

    for (int i = first; i < argc; i++)
        do_file(argv[i]);

    return 0;
}
