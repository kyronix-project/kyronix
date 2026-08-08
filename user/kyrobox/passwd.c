#define _GNU_SOURCE
#include "common.h"

#include <shadow.h>
#include <sys/random.h>
#include <termios.h>

#define PASSWORD_MAX 256
#define HASH_MAX 512

static void wipe(void *ptr, size_t len) {
    volatile unsigned char *p = ptr;
    while (len-- > 0) *p++ = 0;
}

static int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_secret(int fd, const char *prompt, char *buf, size_t size) {
    struct termios saved;
    if (size < 2 || tcgetattr(fd, &saved) < 0) return -1;

    struct termios input = saved;
    input.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG);
    input.c_cc[VMIN] = 1;
    input.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSAFLUSH, &input) < 0) return -1;

    int result = 0;
    int overflow = 0;
    size_t used = 0;
    if (write_all(fd, prompt, strlen(prompt)) < 0) result = -1;

    while (result == 0) {
        unsigned char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            result = -1;
            break;
        }
        if (n == 0 || c == 3 || (c == 4 && used == 0)) {
            errno = EINTR;
            result = -1;
            break;
        }
        if (c == '\n' || c == '\r') break;
        if (c == 0x7f || c == '\b') {
            if (used > 0) used--;
            continue;
        }
        if (c == 0x15) {
            used = 0;
            overflow = 0;
            continue;
        }
        if (used + 1 < size)
            buf[used++] = (char)c;
        else
            overflow = 1;
    }

    buf[used] = '\0';
    (void)write_all(fd, "\n", 1);
    if (tcsetattr(fd, TCSAFLUSH, &saved) < 0) result = -1;
    if (overflow) {
        errno = EOVERFLOW;
        result = -1;
    }
    return result;
}

static int secure_equal(const char *a, const char *b) {
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    size_t max = alen > blen ? alen : blen;
    unsigned char diff = (unsigned char)(alen ^ blen);
    for (size_t i = 0; i < max; i++) {
        unsigned char ac = i < alen ? (unsigned char)a[i] : 0;
        unsigned char bc = i < blen ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ac ^ bc);
    }
    return diff == 0;
}

static int current_hash(const char *user, char *out, size_t out_size) {
    const char *hash = NULL;
    struct spwd *sp = getspnam(user);
    if (sp) hash = sp->sp_pwdp;

    if (!hash) {
        struct passwd *pw = getpwnam(user);
        if (pw) hash = pw->pw_passwd;
    }
    if (!hash || strcmp(hash, "x") == 0 || strlen(hash) >= out_size) return -1;
    strcpy(out, hash);
    return 0;
}

static int verify_password(const char *password, const char *expected) {
    if (expected[0] == '\0') return password[0] == '\0';
    if (expected[0] == '!' || expected[0] == '*') return 0;

    char *calculated = crypt(password, expected);
    return calculated && secure_equal(calculated, expected);
}

static int make_hash(const char *password, char *out, size_t out_size) {
    static const char alphabet[] =
        "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char random[16];
    size_t done = 0;
    while (done < sizeof(random)) {
        ssize_t n = getrandom(random + done, sizeof(random) - done, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }

    char salt[21] = "$6$";
    for (size_t i = 0; i < sizeof(random); i++)
        salt[3 + i] = alphabet[random[i] & 63U];
    salt[19] = '$';
    salt[20] = '\0';
    wipe(random, sizeof(random));

    char *hash = crypt(password, salt);
    wipe(salt, sizeof(salt));
    if (!hash || hash[0] == '*' || strlen(hash) >= out_size) {
        errno = EIO;
        return -1;
    }
    strcpy(out, hash);
    return 0;
}

static int rewrite_shadow(const char *user, const char *hash) {
    int result = -1;
    int input_fd = -1;
    int output_fd = -1;
    FILE *input = NULL;
    FILE *output = NULL;
    char *line = NULL;
    size_t capacity = 0;
    int found = 0;
    long today = (long)(time(NULL) / 86400);
    char temporary[64];

    if (snprintf(temporary, sizeof(temporary), "/etc/.shadow.passwd.%ld",
                 (long)getpid()) >= (int)sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    input_fd = open("/etc/shadow", O_RDONLY | O_NOFOLLOW);
    if (input_fd < 0) goto out;
    struct stat st;
    if (fstat(input_fd, &st) < 0) goto out;
    if (!S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022) != 0) {
        errno = EPERM;
        goto out;
    }
    input = fdopen(input_fd, "r");
    if (!input) goto out;
    input_fd = -1;

    output_fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (output_fd < 0) goto out;
    if (fchmod(output_fd, 0600) < 0) goto out;
    output = fdopen(output_fd, "w");
    if (!output) goto out;
    output_fd = -1;

    ssize_t length;
    while ((length = getline(&line, &capacity, input)) >= 0) {
        char *first = strchr(line, ':');
        if (first && (size_t)(first - line) == strlen(user) &&
            memcmp(line, user, strlen(user)) == 0) {
            char *second = strchr(first + 1, ':');
            char *third = second ? strchr(second + 1, ':') : NULL;
            if (!second || !third || found) {
                errno = EINVAL;
                goto out;
            }
            if (fwrite(line, 1, (size_t)(first - line + 1), output) !=
                    (size_t)(first - line + 1) ||
                fputs(hash, output) == EOF ||
                fprintf(output, ":%ld", today) < 0 || fputs(third, output) == EOF)
                goto out;
            found = 1;
        } else if (fwrite(line, 1, (size_t)length, output) != (size_t)length) {
            goto out;
        }
    }
    if (ferror(input)) goto out;

    if (!found) {
        if (fprintf(output, "%s:%s:%ld:0:99999:7:::\n", user, hash, today) < 0)
            goto out;
    }
    if (fflush(output) < 0 || fsync(fileno(output)) < 0) goto out;
    if (fclose(output) < 0) {
        output = NULL;
        goto out;
    }
    output = NULL;
    if (rename(temporary, "/etc/shadow") < 0) goto out;
    result = 0;

out:
    {
        int saved_errno = errno;
        free(line);
        if (input) fclose(input);
        if (output) fclose(output);
        if (input_fd >= 0) close(input_fd);
        if (output_fd >= 0) close(output_fd);
        if (result < 0) unlink(temporary);
        errno = saved_errno;
    }
    return result;
}

static void usage(void) {
    fprintf(stderr, "usage: passwd [user]\n");
}

int main(int argc, char **argv) {
    kx_prog = "passwd";
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        usage();
        return 2;
    }

    uid_t real_uid = getuid();
    if (geteuid() != 0) {
        fprintf(stderr, "passwd: must be installed setuid root\n");
        return 1;
    }

    struct passwd *caller = getpwuid(real_uid);
    if (!caller) {
        fprintf(stderr, "passwd: cannot identify caller uid %lu\n", (unsigned long)real_uid);
        return 1;
    }
    char caller_name[64];
    if (strlen(caller->pw_name) >= sizeof(caller_name)) {
        fprintf(stderr, "passwd: caller name is too long\n");
        return 1;
    }
    strcpy(caller_name, caller->pw_name);

    const char *target_arg = argc == 2 ? argv[1] : caller_name;
    if (real_uid != 0 && strcmp(target_arg, caller_name) != 0) {
        fprintf(stderr, "passwd: only root may change another user's password\n");
        return 1;
    }
    struct passwd *target = getpwnam(target_arg);
    if (!target) {
        fprintf(stderr, "passwd: user '%s' does not exist\n", target_arg);
        return 1;
    }
    char target_name[64];
    if (strlen(target->pw_name) >= sizeof(target_name)) {
        fprintf(stderr, "passwd: user name is too long\n");
        return 1;
    }
    strcpy(target_name, target->pw_name);

    int tty = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (tty < 0) {
        kx_warn("/dev/tty");
        return 1;
    }

    char old_password[PASSWORD_MAX] = {0};
    char new_password[PASSWORD_MAX] = {0};
    char confirmation[PASSWORD_MAX] = {0};
    char expected[HASH_MAX] = {0};
    char hash[HASH_MAX] = {0};
    int status = 1;

    if (real_uid != 0) {
        if (current_hash(target_name, expected, sizeof(expected)) < 0 ||
            expected[0] == '!' || expected[0] == '*') {
            fprintf(stderr, "passwd: account is locked or has no usable password\n");
            goto out;
        }
        if (read_secret(tty, "Current password: ", old_password,
                        sizeof(old_password)) < 0) {
            fprintf(stderr, "passwd: unable to read password\n");
            goto out;
        }
        if (!verify_password(old_password, expected)) {
            fprintf(stderr, "passwd: authentication failed\n");
            goto out;
        }
    }

    if (read_secret(tty, "New password: ", new_password, sizeof(new_password)) < 0 ||
        read_secret(tty, "Retype new password: ", confirmation, sizeof(confirmation)) < 0) {
        fprintf(stderr, "passwd: unable to read password\n");
        goto out;
    }
    if (new_password[0] == '\0') {
        fprintf(stderr, "passwd: empty passwords are not allowed\n");
        goto out;
    }
    if (strcmp(new_password, confirmation) != 0) {
        fprintf(stderr, "passwd: passwords do not match\n");
        goto out;
    }
    if (real_uid != 0 && strlen(new_password) < 8) {
        fprintf(stderr, "passwd: password must contain at least 8 characters\n");
        goto out;
    }
    if (real_uid == 0 && strlen(new_password) < 8)
        fprintf(stderr, "passwd: warning: password is shorter than 8 characters\n");

    if (make_hash(new_password, hash, sizeof(hash)) < 0) {
        kx_warn("cannot hash password");
        goto out;
    }

    if (lckpwdf() < 0) {
        kx_warn("cannot lock password database");
        goto out;
    }
    if (rewrite_shadow(target_name, hash) < 0) {
        int saved_errno = errno;
        ulckpwdf();
        errno = saved_errno;
        kx_warn("/etc/shadow");
        goto out;
    }
    ulckpwdf();

    printf("passwd: password updated successfully\n");
    status = 0;

out:
    wipe(old_password, sizeof(old_password));
    wipe(new_password, sizeof(new_password));
    wipe(confirmation, sizeof(confirmation));
    wipe(expected, sizeof(expected));
    wipe(hash, sizeof(hash));
    close(tty);
    return status;
}
