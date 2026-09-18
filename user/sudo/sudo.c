#define _XOPEN_SOURCE 700
#include <grp.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void putstr(const char *s) { write(STDERR_FILENO, s, strlen(s)); }

static void read_pass(char *buf, size_t size, const char *prompt) {
    putstr(prompt);
    size_t i = 0;
    int c;
    for (;;) {
        c = getchar();
        if (c == EOF || c == '\n' || c == '\r') {
            buf[i] = '\0';
            putstr("\n");
            return;
        }
        if (c == '\b' || c == 0x7f) {
            if (i > 0) {
                i--;
                putstr("\b \b");
            }
            continue;
        }
        if (i < size - 1) buf[i++] = (char) c;
    }
}

static int verify(const char *user, const char *pass) {
    struct spwd *sp = getspnam(user);
    if (sp && sp->sp_pwdp) {
        const char *enc = crypt(pass, sp->sp_pwdp);
        return enc && strcmp(enc, sp->sp_pwdp) == 0;
    }
    struct passwd *pw = getpwnam(user);
    if (!pw || !pw->pw_passwd) return 0;
    if (pw->pw_passwd[0] == '\0') return 1;
    const char *enc = crypt(pass, pw->pw_passwd);
    return enc && strcmp(enc, pw->pw_passwd) == 0;
}

static int in_group(const char *user, const char *gname) {
    struct group *g = getgrnam(gname);
    if (!g) return 0;
    for (char **m = g->gr_mem; *m; m++)
        if (strcmp(*m, user) == 0) return 1;
    struct passwd *pw = getpwnam(user);
    if (pw && pw->pw_gid == g->gr_gid) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        putstr("usage: sudo <command> [args...]\n");
        return 1;
    }

    struct passwd *pw = getpwuid(getuid());
    if (!pw) {
        putstr("sudo: unknown user\n");
        return 1;
    }

    if (geteuid() != 0) {
        if (strcmp(pw->pw_name, "root") != 0 && !in_group(pw->pw_name, "sudo") &&
            !in_group(pw->pw_name, "wheel")) {
            putstr("sudo: user is not in the sudoers file\n");
            return 1;
        }
        char pass[128];
        char prompt[64];
        snprintf(prompt, sizeof(prompt), "[sudo] password for %s: ", pw->pw_name);
        read_pass(pass, sizeof(pass), prompt);
        if (!verify(pw->pw_name, pass)) {
            putstr("sudo: authentication failure\n");
            return 1;
        }
    }

    setenv("SUDO_USER", pw->pw_name, 1);
    setenv("SUDO_UID", "1000", 1);
    setenv("SUDO_GID", "1000", 1);

    setgid(0);
    setuid(0);
    execvp(argv[1], &argv[1]);
    putstr("sudo: command not found\n");
    return 1;
}
