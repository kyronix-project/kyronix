#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <ctype.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define PROMPT_MAX 64
#define LINE_MAX 256

static void putstr(const char *s) { write(STDERR_FILENO, s, strlen(s)); }

static void redraw_line(const char *prompt, const char *buf, size_t length, int echo) {
    putstr("\r\033[2K");
    putstr(prompt);
    if (echo > 0) {
        write(STDERR_FILENO, buf, length);
    } else if (echo < 0) {
        for (size_t j = 0; j < length; j++) putstr("*");
    }
}

static void read_line(const char *prompt, char *buf, size_t size, int echo) {
    struct termios saved;
    int restore_termios = 0;
    if (tcgetattr(STDIN_FILENO, &saved) == 0) {
        struct termios input = saved;
        input.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
        input.c_cc[VMIN] = 1;
        input.c_cc[VTIME] = 0;
        restore_termios = tcsetattr(STDIN_FILENO, TCSAFLUSH, &input) == 0;
    }

    putstr(prompt);
    size_t i = 0;
    int c;
    for (;;) {
        c = getchar();
        if (c == EOF) {
            buf[i] = '\0';
            break;
        }
        if (c == '\n' || c == '\r') {
            buf[i] = '\0';
            putstr("\n");
            break;
        }
        if (c == '\b' || c == 0x7f) {
            if (i > 0) i--;
            redraw_line(prompt, buf, i, echo);
            continue;
        }
        if (i < size - 1) {
            buf[i++] = c;
            if (echo > 0)
                write(STDERR_FILENO, &c, 1);
            else if (echo < 0)
                putstr("*");
        }
    }
    if (restore_termios) tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
}

static void print_issue(void) {
    int fd = open("/etc/issue", O_RDONLY);
    if (fd < 0) return;
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) write(STDERR_FILENO, buf, n);
    close(fd);
    puts("");
}

/* 1 = password required, 0 = intentionally empty, -1 = locked account */
static int password_state(const char *user) {
    struct spwd *sp = getspnam(user);
    if (sp && sp->sp_pwdp) {
        const char *p = sp->sp_pwdp;
        if (p[0] == '*' || p[0] == '!') return -1;
        if (p[0] == '\0') return 0;
        return 1;
    }

    struct passwd *pw = getpwnam(user);
    if (!pw || !pw->pw_passwd) return 0;
    if (pw->pw_passwd[0] == '\0') return 0;
    if (pw->pw_passwd[0] == '*' || pw->pw_passwd[0] == '!') return -1;

    return 1;
}

static int check_password(const char *user, const char *pass) {
    struct spwd *sp = getspnam(user);
    if (sp && sp->sp_pwdp) {
        const char *enc = crypt(pass, sp->sp_pwdp);
        return enc && strcmp(enc, sp->sp_pwdp) == 0;
    }

    struct passwd *pw = getpwnam(user);
    if (!pw || !pw->pw_passwd) return 0;
    if (pw->pw_passwd[0] == '\0') return 1;
    if (strcmp(pw->pw_passwd, "x") == 0) return 0;

    const char *enc = crypt(pass, pw->pw_passwd);
    return enc && strcmp(enc, pw->pw_passwd) == 0;
}

int main(void) {
    struct passwd *pw;
    struct utsname uts;
    int first = 1;

    uname(&uts);

    if (!isatty(STDIN_FILENO)) {
        int fd = open("/dev/tty", O_RDWR);
        if (fd < 0) {
            putstr("login: no tty\n");
            return 1;
        }
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }

    for (;;) {
        setspent();

        for (;;) {
            char user[PROMPT_MAX];
            char pass[LINE_MAX];

            if (first) {
                print_issue();
                first = 0;
            }
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "%.64s login: ", uts.nodename);
            read_line(prompt, user, sizeof(user), 1);
            if (user[0] == '\0') continue;

            pw = getpwnam(user);
            if (!pw) {
                putstr("Login incorrect\n");
                sleep(1);
                continue;
            }

            int pwstate = password_state(user);
            if (pwstate < 0) {
                putstr("Login incorrect\n");
                sleep(1);
                continue;
            }
            if (pwstate == 0) {
                break;
            }

            read_line("Password: ", pass, sizeof(pass), -1);

            if (!check_password(user, pass)) {
                putstr("Login incorrect\n");
                sleep(1);
                continue;
            }

            break;
        }

        endspent();

        setenv("USER", pw->pw_name, 1);
        setenv("HOME", pw->pw_dir, 1);
        setenv("SHELL", pw->pw_shell, 1);
        setenv("LOGNAME", pw->pw_name, 1);
        setenv("TERM", "xterm-color", 1);
        setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);

        if (chdir(pw->pw_dir) < 0) chdir("/");

        initgroups(pw->pw_name, pw->pw_gid);
        setgid(pw->pw_gid);
        setuid(pw->pw_uid);
        execlp(pw->pw_shell, pw->pw_shell, NULL);
        putstr("login: unable to start shell\n");
        return 1;
    }
}
