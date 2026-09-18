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

int main(int argc, char **argv) {
    if (geteuid() != 0) {
        putstr("passwd: only root can change passwords\n");
        return 1;
    }

    char user[64];
    if (argc > 1) {
        strncpy(user, argv[1], sizeof(user) - 1);
        user[sizeof(user) - 1] = 0;
    } else {
        struct passwd *pw = getpwuid(getuid());
        if (!pw) {
            putstr("passwd: unknown user\n");
            return 1;
        }
        strncpy(user, pw->pw_name, sizeof(user) - 1);
        user[sizeof(user) - 1] = 0;
    }

    struct passwd *pw = getpwnam(user);
    if (!pw) {
        putstr("passwd: user not found\n");
        return 1;
    }

    char pass1[128], pass2[128];
    read_pass(pass1, sizeof(pass1), "New password: ");
    read_pass(pass2, sizeof(pass2), "Retype new password: ");
    if (strcmp(pass1, pass2) != 0) {
        putstr("passwd: passwords do not match\n");
        return 1;
    }
    if (strlen(pass1) < 4) {
        putstr("passwd: password too short\n");
        return 1;
    }

    struct spwd *sp = getspnam(user);
    if (!sp) {
        putstr("passwd: no shadow entry\n");
        return 1;
    }

    const char *hash = crypt(pass1, sp->sp_pwdp);
    if (!hash) {
        putstr("passwd: crypt failed\n");
        return 1;
    }

    FILE *f = fopen("/etc/shadow", "r");
    if (!f) {
        putstr("passwd: cannot open /etc/shadow\n");
        return 1;
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;

    char out[4096];
    size_t out_pos = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *colon = strchr(line, ':');
        if (colon) *colon = 0;
        if (strcmp(line, user) == 0) {
            *colon = ':';
            char *rest = colon + 1;
            char *next = strchr(rest, ':');
            if (next) *next = 0;
            out_pos += snprintf(out + out_pos, sizeof(out) - out_pos, "%s:%s:%s\n", user, hash,
                                next ? next + 1 : "");
        } else {
            *colon = ':';
            out_pos += snprintf(out + out_pos, sizeof(out) - out_pos, "%s\n", line);
        }
    }

    f = fopen("/etc/shadow", "w");
    if (!f) {
        putstr("passwd: cannot write /etc/shadow\n");
        return 1;
    }
    fwrite(out, 1, out_pos, f);
    fclose(f);

    putstr("passwd: password updated successfully\n");
    return 0;
}
