#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

int main(void)
{
    int p[2];
    if (pipe(p) < 0) { fprintf(stderr, "pipe: %s\n", strerror(errno)); return 1; }
    fprintf(stderr, "pipe: [%d, %d]\n", p[0], p[1]);

    pid_t pid = fork();
    if (pid < 0) { fprintf(stderr, "fork: %s\n", strerror(errno)); return 1; }

    if (pid == 0)
    {
        close(p[0]);
        int r = dup2(p[1], STDOUT_FILENO);
        fprintf(stderr, "child: dup2(%d,1)=%d errno=%d\n", p[1], r, errno);
        if (r < 0) _exit(1);
        close(p[1]);
        execlp("ls", "ls", "/bin", NULL);
        fprintf(stderr, "child: exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    close(p[1]);
    char buf[4096];
    ssize_t n;
    fprintf(stderr, "parent: reading from pipe...\n");
    while ((n = read(p[0], buf, sizeof(buf))) > 0)
    {
        fprintf(stderr, "parent: read %zd bytes\n", n);
        write(STDOUT_FILENO, buf, n);
    }
    fprintf(stderr, "parent: read done (%zd), waiting for child...\n", n);
    int status;
    waitpid(pid, &status, 0);
    fprintf(stderr, "parent: child exited status=%d\n", status);
    close(p[0]);
    return 0;
}
