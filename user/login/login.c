#define _POSIX_C_SOURCE 200809
#include <crypt.h>
#include <ctype.h>
#include <fcntl.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define PROMPT_MAX 64
#define LINE_MAX 256

static void putstr(const char *s)
{
    write(STDERR_FILENO, s, strlen(s));
}

static void read_line(char *buf, size_t size, int echo)
{
    size_t i = 0;
    int c;
    for (;;) {
        c = getchar();
        if (c == EOF) {
            buf[i] = '\0';
            return;
        }
        if (c == '\n' || c == '\r') {
            buf[i] = '\0';
            putstr("\r\n");
            return;
        }
        if (i < size - 1) {
            buf[i++] = c;
            if (echo)
                write(STDERR_FILENO, &c, 1);
        }
    }
}

static void print_issue(void)
{
    int fd = open("/etc/issue", O_RDONLY);
    if (fd < 0)
        return;
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(STDERR_FILENO, buf, n);
    close(fd);
    puts("");
}

static int check_password(const char *user, const char *pass)
{
    struct spwd *sp = getspnam(user);
    if (sp && sp->sp_pwdp) {
        const char *enc = crypt(pass, sp->sp_pwdp);
        return enc && strcmp(enc, sp->sp_pwdp) == 0;
    }

    struct passwd *pw = getpwnam(user);
    if (!pw || !pw->pw_passwd)
        return 0;
    if (pw->pw_passwd[0] == '\0')
        return 1;
    if (strcmp(pw->pw_passwd, "x") == 0)
        return 0;

    const char *enc = crypt(pass, pw->pw_passwd);
    return enc && strcmp(enc, pw->pw_passwd) == 0;
}

int main(void)
{
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
        if (fd > STDERR_FILENO)
            close(fd);
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
            putstr(uts.nodename);
            putstr(" login: ");
            read_line(user, sizeof(user), 1);
            if (user[0] == '\0')
                continue;

            putstr("Password: ");
            read_line(pass, sizeof(pass), 0);

            pw = getpwnam(user);
            if (!pw) {
                putstr("Login incorrect\n");
                sleep(1);
                continue;
            }

            if (!check_password(user, pass)) {
                putstr("Login incorrect\n");
                sleep(1);
                continue;
            }

            break;
        }

        endspent();

        pid_t pid = fork();
        if (pid < 0) {
            putstr("login: fork failed\n");
            continue;
        }

        if (pid == 0) {
            setenv("USER", pw->pw_name, 1);
            setenv("HOME", pw->pw_dir, 1);
            setenv("SHELL", pw->pw_shell, 1);
            setenv("LOGNAME", pw->pw_name, 1);
            setenv("TERM", "vt100", 1);

            if (chdir(pw->pw_dir) < 0)
                chdir("/");

            setsid();
            setgid(pw->pw_gid);
            setuid(pw->pw_uid);
            execlp(pw->pw_shell, pw->pw_shell, NULL);
            putstr("login: unable to start shell\n");
            _exit(1);
        }

        {
            int status;
            waitpid(pid, &status, 0);
        }
    }
}
