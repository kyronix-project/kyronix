#define _POSIX_C_SOURCE 200809
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_ARGS 32
#define MAX_LINE 512
#define MAX_HISTORY 64

static void print_matches_columns(char names[][PATH_MAX], int count)
{
    struct winsize ws;
    ws.ws_col = 80;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    int term_w = (ws.ws_col > 0) ? (int) ws.ws_col : 80;

    int max_w = 0;
    for (int i = 0; i < count; i++)
    {
        int w = (int) strlen(names[i]);
        if (w > max_w) max_w = w;
    }

    int col_w = max_w + 2;
    int num_cols = term_w / col_w;
    if (num_cols < 1) num_cols = 1;
    int num_rows = (count + num_cols - 1) / num_cols;

    for (int row = 0; row < num_rows; row++)
    {
        for (int col = 0; col < num_cols; col++)
        {
            int idx = col * num_rows + row;
            if (idx >= count) break;
            int w = (int) strlen(names[idx]);
            fputs(names[idx], stdout);
            if (col < num_cols - 1 && (col + 1) * num_rows + row < count)
                printf("%-*s", col_w - w, "");
        }
        putchar('\n');
    }
}
static int cached_uid = -1;
static char shell_pwd[MAX_LINE];

static void build_prompt(char* buf, size_t size)
{
    static char cwd[MAX_LINE];
    const char* cwd_str = getcwd(cwd, sizeof(cwd));
    if (cwd_str == NULL)
    {
        cwd_str = shell_pwd;
        if (cwd_str[0] == '\0')
        {
            cwd_str = "?";
        }
    }

    const char* display_path = cwd_str;
    static char tilde_path[MAX_LINE];
    const char* home = getenv("HOME");
    if (home != NULL)
    {
        size_t home_len = strlen(home);
        if (strncmp(cwd_str, home, home_len) == 0)
        {
            if (cwd_str[home_len] == '\0')
            {
                display_path = "~";
            }
            else if (cwd_str[home_len] == '/')
            {
                snprintf(tilde_path, sizeof(tilde_path), "~%s", cwd_str + home_len);
                display_path = tilde_path;
            }
        }
    }

    const char* prompt_char = (cached_uid == 0) ? "#" : "$";
    snprintf(buf, size, "\033[34m%s\033[0m %s ", display_path, prompt_char);
}

static struct termios saved_termios;
static int termios_saved = 0;

static char history[MAX_HISTORY][MAX_LINE];
static int history_count = 0;
static int history_start = 0;

static int split_line(char* line, char** argv)
{
    int argc = 0;
    char* cursor = line;

    while (*cursor != '\0' && argc < MAX_ARGS - 1)
    {
        while (isspace((unsigned char) *cursor))
        {
            cursor++;
        }
        if (*cursor == '\0')
        {
            break;
        }

        argv[argc++] = cursor;
        while (*cursor != '\0' && !isspace((unsigned char) *cursor))
        {
            cursor++;
        }
        if (*cursor != '\0')
        {
            *cursor++ = '\0';
        }
    }

    argv[argc] = NULL;
    return argc;
}

static const char* history_path(void)
{
    const char* home = getenv("HOME");
    return home != NULL ? home : "/root";
}

static void history_load(void)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.ksh_history", history_path());

    FILE* file = fopen(path, "r");
    if (file == NULL)
    {
        return;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), file) != NULL)
    {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0')
        {
            continue;
        }
        if (history_count < MAX_HISTORY)
        {
            snprintf(history[history_count], MAX_LINE, "%s", line);
            history_count++;
        }
        else
        {
            snprintf(history[history_start], MAX_LINE, "%s", line);
            history_start = (history_start + 1) % MAX_HISTORY;
        }
    }

    fclose(file);
}

static void history_save(void)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.ksh_history", history_path());

    FILE* file = fopen(path, "w");
    if (file == NULL)
    {
        return;
    }

    for (int i = 0; i < history_count; i++)
    {
        int index = (history_start + i) % MAX_HISTORY;
        fprintf(file, "%s\n", history[index]);
    }

    fclose(file);
}

static void history_add(const char* line)
{
    if (line[0] == '\0')
    {
        return;
    }

    if (history_count > 0)
    {
        int last = (history_start + history_count - 1) % MAX_HISTORY;
        if (strcmp(history[last], line) == 0)
        {
            return;
        }
    }

    if (history_count < MAX_HISTORY)
    {
        snprintf(history[history_count], MAX_LINE, "%s", line);
        history_count++;
    }
    else
    {
        snprintf(history[history_start], MAX_LINE, "%s", line);
        history_start = (history_start + 1) % MAX_HISTORY;
    }

    history_save();
}

static void terminal_enable_raw(void)
{
    if (!isatty(STDIN_FILENO))
    {
        return;
    }

    if (tcgetattr(STDIN_FILENO, &saved_termios) == -1)
    {
        return;
    }

    struct termios raw = saved_termios;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    termios_saved = 1;
}

static void terminal_restore(void)
{
    if (termios_saved)
    {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
        termios_saved = 0;
    }
}

static int read_byte(void)
{
    unsigned char byte = 0;
    ssize_t r;
    do {
        r = read(STDIN_FILENO, &byte, 1);
    } while (r < 0 && errno == EINTR);
    return (r == 1) ? (int) byte : -1;
}

static int read_escape_sequence(void)
{
    int next = read_byte();
    if (next != '[')
    {
        return -1;
    }

    next = read_byte();
    if (next == 'A')
    {
        return 256;
    }
    if (next == 'B')
    {
        return 257;
    }
    if (next == 'C')
    {
        return 258;
    }
    if (next == 'D')
    {
        return 259;
    }
    if (next == '3')
    {
        if (read_byte() == '~')
        {
            return 127;
        }
    }

    return -1;
}

static void redraw_line(const char* line, size_t cursor)
{
    char prompt[PATH_MAX + 64];
    build_prompt(prompt, sizeof(prompt));

    fputs("\r\033[K", stdout);
    fputs(prompt, stdout);
    fputs(line, stdout);
    fflush(stdout);
    if (cursor < strlen(line))
    {
        dprintf(STDOUT_FILENO, "\033[%zuD", strlen(line) - cursor);
    }
}

static int common_prefix_len(const char* a, const char* b)
{
    size_t i = 0;
    while (a[i] != '\0' && b[i] != '\0' && a[i] == b[i])
    {
        i++;
    }
    return (int) i;
}

static void complete_token(char* line, size_t* cursor, size_t* length)
{
    size_t end = *cursor;
    size_t start = end;
    while (start > 0 && !isspace((unsigned char) line[start - 1]))
    {
        start--;
    }

    char token[MAX_LINE];
    size_t token_len = end - start;
    if (token_len >= sizeof(token))
    {
        return;
    }
    memcpy(token, line + start, token_len);
    token[token_len] = '\0';

    int completing_command = (start == 0);
    char matches[64][PATH_MAX];
    int match_count = 0;

    if (completing_command)
    {
        const char* path_env = getenv("PATH");
        if (path_env != NULL)
        {
            char path_copy[PATH_MAX];
            strncpy(path_copy, path_env, sizeof(path_copy) - 1);
            path_copy[sizeof(path_copy) - 1] = '\0';

            char* save = NULL;
            for (char* dir = strtok_r(path_copy, ":", &save); dir != NULL && match_count < 64;
                 dir = strtok_r(NULL, ":", &save))
            {
                DIR* directory = opendir(dir);
                if (directory == NULL)
                {
                    continue;
                }

                struct dirent* entry = NULL;
                while ((entry = readdir(directory)) != NULL && match_count < 64)
                {
                    if (entry->d_name[0] == '.')
                    {
                        continue;
                    }
                    if (strncmp(entry->d_name, token, token_len) != 0)
                    {
                        continue;
                    }

                    int duplicate = 0;
                    for (int i = 0; i < match_count; i++)
                    {
                        if (strcmp(matches[i], entry->d_name) == 0)
                        {
                            duplicate = 1;
                            break;
                        }
                    }
                    if (duplicate)
                    {
                        continue;
                    }

                    char candidate[PATH_MAX];
                    snprintf(candidate, sizeof(candidate), "%s/%s", dir, entry->d_name);
                    if (access(candidate, X_OK) != 0)
                    {
                        continue;
                    }

                    snprintf(matches[match_count], sizeof(matches[match_count]), "%s",
                             entry->d_name);
                    match_count++;
                }
                closedir(directory);
            }
        }
    }
    else
    {
        char dirpart[PATH_MAX];
        const char* base = token;
        char* slash = strrchr(token, '/');
        if (slash != NULL)
        {
            size_t dir_len = (size_t) (slash - token);
            if (dir_len == 0)
            {
                strcpy(dirpart, "/");
            }
            else
            {
                memcpy(dirpart, token, dir_len);
                dirpart[dir_len] = '\0';
            }
            base = slash + 1;
        }
        else
        {
            if (!getcwd(dirpart, sizeof(dirpart)))
            {
                return;
            }
        }

        DIR* directory = opendir(dirpart);
        if (directory == NULL)
        {
            return;
        }

        size_t base_len = strlen(base);
        struct dirent* entry = NULL;
        while ((entry = readdir(directory)) != NULL && match_count < 64)
        {
            if (entry->d_name[0] == '.' && base_len == 0)
            {
                continue;
            }
            if (strncmp(entry->d_name, base, base_len) != 0)
            {
                continue;
            }

            char full[PATH_MAX];
            if (strcmp(dirpart, "/") == 0)
            {
                snprintf(full, sizeof(full), "/%s", entry->d_name);
            }
            else
            {
                snprintf(full, sizeof(full), "%s/%s", dirpart, entry->d_name);
            }

            struct stat st;
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
            {
                strncat(full, "/", sizeof(full) - strlen(full) - 1);
            }

            if (slash != NULL)
            {
                snprintf(matches[match_count], sizeof(matches[match_count]), "%s", full);
            }
            else
            {
                snprintf(matches[match_count], sizeof(matches[match_count]), "%s", full);
            }
            match_count++;
        }
        closedir(directory);
    }

    if (match_count == 0)
    {
        return;
    }

    if (match_count == 1)
    {
        const char* replacement = matches[0];
        size_t replace_len = strlen(replacement);
        if (*length + replace_len > token_len + (MAX_LINE - 1))
        {
            return;
        }

        memmove(line + start + replace_len, line + end, *length - end + 1);
        memcpy(line + start, replacement, replace_len);
        *length = start + replace_len + (*length - end);
        *cursor = *length;
        redraw_line(line, *cursor);
        return;
    }

    int prefix = (int) strlen(matches[0]);
    for (int i = 1; i < match_count; i++)
    {
        prefix = common_prefix_len(matches[0], matches[i]);
        if (prefix <= (int) token_len)
        {
            prefix = (int) token_len;
            break;
        }
    }

    if (prefix > (int) token_len)
    {
        size_t add = (size_t) prefix - token_len;
        if (*length + add >= MAX_LINE - 1)
        {
            return;
        }
        memmove(line + start + prefix, line + end, *length - end + 1);
        memcpy(line + start, matches[0], (size_t) prefix);
        *length = start + prefix + (*length - end);
        *cursor = *length;
        redraw_line(line, *cursor);
    }

    putchar('\n');
    print_matches_columns(matches, match_count);
    redraw_line(line, *cursor);
}

static int read_line(char* line, size_t size)
{
    if (!isatty(STDIN_FILENO))
    {
        if (fgets(line, (int) size, stdin) == NULL)
        {
            return -1;
        }
        line[strcspn(line, "\n")] = '\0';
        return 0;
    }

    terminal_enable_raw();

    size_t length = 0;
    size_t cursor = 0;
    int history_index = history_count;

    line[0] = '\0';

    char prompt[PATH_MAX + 64];
    build_prompt(prompt, sizeof(prompt));
    fputs(prompt, stdout);
    fflush(stdout);

    for (;;)
    {
        int key = read_byte();
        if (key == -1)
        {
            terminal_restore();
            return -1;
        }

        if (key == '\n' || key == '\r')
        {
            putchar('\n');
            line[length] = '\0';
            terminal_restore();
            return 0;
        }

        if (key == 3) /* Ctrl+C: cancel current line */
        {
            write(STDERR_FILENO, "^C\n", 3);
            terminal_restore();
            return 2;
        }

        if (key == 4)
        {
            terminal_restore();
            return -1; /* EOF */
        }

        if (key == 127 || key == 8)
        {
            if (cursor > 0)
            {
                memmove(line + cursor - 1, line + cursor, length - cursor + 1);
                length--;
                cursor--;
                redraw_line(line, cursor);
            }
            continue;
        }

        if (key == '\t')
        {
            complete_token(line, &cursor, &length);
            continue;
        }

        if (key == 27)
        {
            key = read_escape_sequence();
            if (key == 256 && history_count > 0)
            {
                if (history_index > 0)
                {
                    history_index--;
                }
                int index = (history_start + history_index) % MAX_HISTORY;
                strncpy(line, history[index], size - 1);
                line[size - 1] = '\0';
                length = strlen(line);
                cursor = length;
                redraw_line(line, cursor);
            }
            else if (key == 257 && history_index < history_count)
            {
                history_index++;
                if (history_index == history_count)
                {
                    line[0] = '\0';
                }
                else
                {
                    int index = (history_start + history_index) % MAX_HISTORY;
                    strncpy(line, history[index], size - 1);
                    line[size - 1] = '\0';
                }
                length = strlen(line);
                cursor = length;
                redraw_line(line, cursor);
            }
            else if (key == 258 && cursor < length)
            {
                cursor++;
                redraw_line(line, cursor);
            }
            else if (key == 259 && cursor > 0)
            {
                cursor--;
                redraw_line(line, cursor);
            }
            continue;
        }

        if (key >= 32 && length + 1 < size - 1)
        {
            memmove(line + cursor + 1, line + cursor, length - cursor + 1);
            line[cursor] = (char) key;
            length++;
            cursor++;
            line[length] = '\0';
            redraw_line(line, cursor);
        }
    }
}

static int exec_pipeline(char** argv, int argc)
{
    int pipe_at[32];
    int n_pipes = 0;
    for (int i = 0; i < argc; i++)
    {
        if (strcmp(argv[i], "|") == 0)
            pipe_at[n_pipes++] = i;
    }
    int n_stages = n_pipes + 1;

    int st_start[32], st_end[32];
    char* outfile[32];
    int append[32];
    int prev = 0;
    for (int s = 0; s < n_stages; s++)
    {
        int end = (s < n_pipes) ? pipe_at[s] : argc;
        st_start[s] = prev;
        st_end[s] = end;
        outfile[s] = NULL;
        append[s] = 0;
        for (int i = prev; i < end; i++)
        {
            if (strcmp(argv[i], ">") == 0 && i + 1 < end)
            {
                outfile[s] = argv[i + 1];
                append[s] = 0;
                st_end[s] = i;
                break;
            }
            if (strcmp(argv[i], ">>") == 0 && i + 1 < end)
            {
                outfile[s] = argv[i + 1];
                append[s] = 1;
                st_end[s] = i;
                break;
            }
        }
        argv[st_end[s]] = NULL;
        prev = end + 1;
    }

    sigset_t sigmask, oldmask;
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGINT);
    sigprocmask(SIG_BLOCK, &sigmask, &oldmask);

    int prev_read = -1;
    pid_t children[32];
    int n_children = 0;

    for (int s = 0; s < n_stages; s++)
    {
        int pipe_w[2] = {-1, -1};
        if (s < n_stages - 1 && pipe(pipe_w) < 0)
        {
            perror("pipe");
            sigprocmask(SIG_SETMASK, &oldmask, NULL);
            return 1;
        }

        pid_t pid = fork();
        if (pid == -1)
        {
            perror("fork");
            sigprocmask(SIG_SETMASK, &oldmask, NULL);
            return 1;
        }
        if (pid == 0)
        {
            signal(SIGINT, SIG_DFL);
            sigprocmask(SIG_SETMASK, &oldmask, NULL);

            if (prev_read >= 0)
            {
                dup2(prev_read, STDIN_FILENO);
                close(prev_read);
            }
            if (pipe_w[1] >= 0)
            {
                close(pipe_w[0]);
                dup2(pipe_w[1], STDOUT_FILENO);
                close(pipe_w[1]);
            }
            if (outfile[s] != NULL)
            {
                int flags = O_WRONLY | O_CREAT | (append[s] ? O_APPEND : O_TRUNC);
                int fd = open(outfile[s], flags, 0666);
                if (fd < 0)
                {
                    perror(outfile[s]);
                    _exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            setpgid(0, 0);
            tcsetpgrp(0, getpgrp());
            execvp(argv[st_start[s]], argv + st_start[s]);
            perror(argv[st_start[s]]);
            _exit(127);
        }

        children[n_children++] = pid;

        if (prev_read >= 0)
            close(prev_read);
        if (pipe_w[1] >= 0)
            close(pipe_w[1]);
        prev_read = pipe_w[0];
    }

    if (prev_read >= 0)
        close(prev_read);

    tcsetpgrp(0, children[0]);
    sigprocmask(SIG_SETMASK, &oldmask, NULL);

    int status = 0;
    for (int i = 0; i < n_children; i++)
    {
        if (waitpid(children[i], &status, 0) == -1)
            perror("waitpid");
    }
    tcsetpgrp(0, getpgrp());

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static void expand_globs(int* argc, char** argv)
{
    static char expanded[MAX_ARGS][MAX_LINE];
    int new_argc = 0;

    for (int i = 0; i < *argc && new_argc < MAX_ARGS - 1; i++)
    {
        glob_t gl;
        if (glob(argv[i], GLOB_NOCHECK | GLOB_TILDE, NULL, &gl) != 0)
        {
            strncpy(expanded[new_argc], argv[i], MAX_LINE - 1);
            expanded[new_argc][MAX_LINE - 1] = '\0';
            new_argc++;
            continue;
        }

        for (size_t j = 0; j < gl.gl_pathc && new_argc < MAX_ARGS - 1; j++)
        {
            strncpy(expanded[new_argc], gl.gl_pathv[j], MAX_LINE - 1);
            expanded[new_argc][MAX_LINE - 1] = '\0';
            new_argc++;
        }
        globfree(&gl);
    }

    for (int i = 0; i < new_argc; i++)
    {
        argv[i] = expanded[i];
    }
    argv[new_argc] = NULL;
    *argc = new_argc;
}

static int resolve_path(const char* target, char* result, size_t result_size)
{
    static char buf[PATH_MAX * 3];

    if (target[0] == '~')
    {
        const char* home = getenv("HOME");
        if (home == NULL)
            return -1;
        size_t home_len = strlen(home);
        size_t rest_len = strlen(target + 1);
        if (home_len + rest_len + 1 > sizeof(buf))
            return -1;
        memcpy(buf, home, home_len);
        memcpy(buf + home_len, target + 1, rest_len + 1);
        target = buf;
    }

    char* path = buf + PATH_MAX;
    if (target[0] == '/')
    {
        size_t len = strlen(target);
        if (len >= PATH_MAX)
            return -1;
        memcpy(path, target, len + 1);
    }
    else
    {
        int n = snprintf(path, PATH_MAX, "%s/%s", shell_pwd, target);
        if (n < 0 || (size_t)n >= PATH_MAX)
            return -1;
    }

    char* norm = buf + PATH_MAX * 2;
    size_t pos = 0;
    const char* p = path;

    while (*p != '\0')
    {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;

        const char* start = p;
        while (*p != '\0' && *p != '/')
            p++;
        size_t comp_len = (size_t)(p - start);

        if (comp_len == 1 && start[0] == '.')
            continue;

        if (comp_len == 2 && start[0] == '.' && start[1] == '.')
        {
            if (pos > 1)
            {
                pos--;
                while (pos > 0 && norm[pos - 1] != '/')
                    pos--;
            }
            norm[pos] = '\0';
            continue;
        }

        if (pos > 0 && norm[pos - 1] != '/')
            norm[pos++] = '/';
        else if (pos == 0)
            norm[pos++] = '/';

        if (pos + comp_len >= PATH_MAX)
            return -1;
        memcpy(norm + pos, start, comp_len);
        pos += comp_len;
        norm[pos] = '\0';
    }

    if (pos == 0)
    {
        norm[pos++] = '/';
        norm[pos] = '\0';
    }

    strncpy(result, norm, result_size);
    result[result_size - 1] = '\0';
    return 0;
}

static void print_help(void)
{
    puts("Built-in commands:");
    puts("  cd [dir]  - Change directory (default: HOME)");
    puts("  exit      - Exit the shell");
    puts("  help      - Display this help message");
    puts("External commands are also supported via $PATH.");
}

static int run_command(int argc, char** argv)
{
    if (argc == 0)
    {
        return 0;
    }

    if (strcmp(argv[0], "exit") == 0)
    {
        exit(0);
    }

    if (strcmp(argv[0], "help") == 0)
    {
        print_help();
        return 0;
    }

    if (strcmp(argv[0], "cd") == 0)
    {
        for (int i = 1; i < argc; i++)
        {
            if (strcmp(argv[i], "|") == 0 || strcmp(argv[i], ">") == 0 || strcmp(argv[i], ">>") == 0)
            {
                fputs("cd: pipes/redirections not supported\n", stderr);
                return 1;
            }
        }
        const char* dir = argv[1] != NULL ? argv[1] : getenv("HOME");
        if (dir == NULL)
        {
            fputs("cd: HOME not set\n", stderr);
            return 0;
        }
        static char resolved[PATH_MAX];
        if (resolve_path(dir, resolved, sizeof(resolved)) != 0)
        {
            fputs("cd: path too long\n", stderr);
            return 0;
        }
        if (chdir(resolved) != 0)
        {
            perror("cd");
            return 0;
        }
        strncpy(shell_pwd, resolved, sizeof(shell_pwd) - 1);
        shell_pwd[sizeof(shell_pwd) - 1] = '\0';
        return 0;
    }

    return exec_pipeline(argv, argc);
}

int main(void)
{
    signal(SIGINT, SIG_IGN);
    puts("");
    puts("Type 'help' for commands.");

    if (getcwd(shell_pwd, sizeof(shell_pwd)) == NULL)
    {
        const char* env = getenv("PWD");
        if (env != NULL)
        {
            strncpy(shell_pwd, env, sizeof(shell_pwd) - 1);
            shell_pwd[sizeof(shell_pwd) - 1] = '\0';
        }
        else
        {
            snprintf(shell_pwd, sizeof(shell_pwd), "/");
        }
    }

    history_load();

    char line[MAX_LINE];
    char* argv[MAX_ARGS];

    for (;;)
    {
        int rl = read_line(line, sizeof(line));
        if (rl == 2)
            continue;
        if (rl != 0)   /* EOF or error */
        {
            putchar('\n');
            break;
        }

        char line_copy[MAX_LINE];
        strncpy(line_copy, line, sizeof(line_copy) - 1);
        line_copy[sizeof(line_copy) - 1] = '\0';

        int argc = split_line(line_copy, argv);
        if (argc > 0)
        {
            history_add(line);
            expand_globs(&argc, argv);
        }
        run_command(argc, argv);
    }

    terminal_restore();
    return 0;
}
