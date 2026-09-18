#define _XOPEN_SOURCE 700
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int main(int argc, char **argv) {
    const char *user = "root";
    const char *shell = NULL;
    int optind = 1;

    while (optind < argc && argv[optind][0] == '-') {
        if (strcmp(argv[optind], "-s") == 0 && optind + 1 < argc) {
            shell = argv[optind + 1];
            optind += 2;
        } else if (strcmp(argv[optind], "-") == 0) {
            optind++;
        } else {
            break;
        }
    }
    if (optind < argc) user = argv[optind];

    struct passwd *pw = getpwnam(user);
    if (!pw) {
        putstr("su: unknown user\n");
        return 1;
    }
    if (!shell) shell = pw->pw_shell;

    if (geteuid() != 0) {
        char pass[128];
        char prompt[64];
        snprintf(prompt, sizeof(prompt), "Password for %s: ", user);
        read_pass(pass, sizeof(pass), prompt);
        if (!verify(user, pass)) {
            putstr("su: authentication failure\n");
            return 1;
        }
    }

    setenv("USER", pw->pw_name, 1);
    setenv("HOME", pw->pw_dir, 1);
    setenv("SHELL", shell, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    if (chdir(pw->pw_dir) < 0) chdir("/");
    setgid(pw->pw_gid);
    setuid(pw->pw_uid);
    execlp(shell, shell, NULL);
    putstr("su: unable to start shell\n");
    return 1;
}
