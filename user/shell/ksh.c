#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_ARGS 32
#define MAX_LINE 512
#define MAX_HISTORY 64

static char cached_hostname[64];
static char cached_username[64];
static int cached_uid = -1;

static void build_prompt(char *buf, size_t size)
{
    if (cached_hostname[0] == '\0') {
        if (gethostname(cached_hostname, sizeof(cached_hostname)) != 0) {
            snprintf(cached_hostname, sizeof(cached_hostname), "kyronix");
        }
        cached_hostname[sizeof(cached_hostname) - 1] = '\0';
    }

    if (cached_uid < 0) {
        cached_uid = (int)getuid();
        struct passwd *pw = getpwuid((uid_t)cached_uid);
        if (pw != NULL && pw->pw_name != NULL) {
            snprintf(cached_username, sizeof(cached_username), "%s", pw->pw_name);
        } else {
            snprintf(cached_username, sizeof(cached_username), "unknown");
        }
    }

    char cwd[PATH_MAX];
    const char *cwd_str = getcwd(cwd, sizeof(cwd));
    if (cwd_str == NULL) {
        cwd_str = "?";
    }

    const char *base = strrchr(cwd_str, '/');
    if (base != NULL && base[1] != '\0') {
        base++;
    } else {
        base = cwd_str;
    }

    const char *prompt_char = (cached_uid == 0) ? "#" : "$";
    const char *prompt_color = (cached_uid == 0) ? "\033[1;31m" : "";

    snprintf(buf, size, "\033[32m%s\033[0m@\033[36m%s\033[0m:\033[1;34m%s\033[0m %s%s\033[0m ",
             cached_username, cached_hostname, base, prompt_color, prompt_char);
}

static struct termios saved_termios;
static int termios_saved = 0;

static char history[MAX_HISTORY][MAX_LINE];
static int history_count = 0;
static int history_start = 0;

static int split_line(char *line, char **argv)
{
    int argc = 0;
    char *cursor = line;

    while (*cursor != '\0' && argc < MAX_ARGS - 1) {
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        argv[argc++] = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor != '\0') {
            *cursor++ = '\0';
        }
    }

    argv[argc] = NULL;
    return argc;
}

static const char *history_path(void)
{
    const char *home = getenv("HOME");
    return home != NULL ? home : "/root";
}

static void history_load(void)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.ksh_history", history_path());

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }
        if (history_count < MAX_HISTORY) {
            snprintf(history[history_count], MAX_LINE, "%s", line);
            history_count++;
        } else {
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

    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return;
    }

    for (int i = 0; i < history_count; i++) {
        int index = (history_start + i) % MAX_HISTORY;
        fprintf(file, "%s\n", history[index]);
    }

    fclose(file);
}

static void history_add(const char *line)
{
    if (line[0] == '\0') {
        return;
    }

    if (history_count > 0) {
        int last = (history_start + history_count - 1) % MAX_HISTORY;
        if (strcmp(history[last], line) == 0) {
            return;
        }
    }

    if (history_count < MAX_HISTORY) {
        snprintf(history[history_count], MAX_LINE, "%s", line);
        history_count++;
    } else {
        snprintf(history[history_start], MAX_LINE, "%s", line);
        history_start = (history_start + 1) % MAX_HISTORY;
    }

    history_save();
}

static void terminal_enable_raw(void)
{
    if (!isatty(STDIN_FILENO)) {
        return;
    }

    if (tcgetattr(STDIN_FILENO, &saved_termios) == -1) {
        return;
    }

    struct termios raw = saved_termios;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    termios_saved = 1;
}

static void terminal_restore(void)
{
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
        termios_saved = 0;
    }
}

static int read_byte(void)
{
    unsigned char byte = 0;
    if (read(STDIN_FILENO, &byte, 1) != 1) {
        return -1;
    }
    return byte;
}

static int read_escape_sequence(void)
{
    int next = read_byte();
    if (next != '[') {
        return -1;
    }

    next = read_byte();
    if (next == 'A') {
        return 256;
    }
    if (next == 'B') {
        return 257;
    }
    if (next == 'C') {
        return 258;
    }
    if (next == 'D') {
        return 259;
    }
    if (next == '3') {
        if (read_byte() == '~') {
            return 127;
        }
    }

    return -1;
}

static void redraw_line(const char *line, size_t cursor)
{
    char prompt[PATH_MAX + 64];
    build_prompt(prompt, sizeof(prompt));

    fputs("\r\033[K", stdout);
    fputs(prompt, stdout);
    fputs(line, stdout);
    if (cursor < strlen(line)) {
        dprintf(STDOUT_FILENO, "\033[%zuD", strlen(line) - cursor);
    }
    fflush(stdout);
}

static int common_prefix_len(const char *a, const char *b)
{
    size_t i = 0;
    while (a[i] != '\0' && b[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return (int)i;
}

static void complete_token(char *line, size_t *cursor, size_t *length)
{
    size_t end = *cursor;
    size_t start = end;
    while (start > 0 && !isspace((unsigned char)line[start - 1])) {
        start--;
    }

    char token[MAX_LINE];
    size_t token_len = end - start;
    if (token_len >= sizeof(token)) {
        return;
    }
    memcpy(token, line + start, token_len);
    token[token_len] = '\0';

    int completing_command = (start == 0);
    char matches[64][PATH_MAX];
    int match_count = 0;

    if (completing_command) {
        const char *path_env = getenv("PATH");
        if (path_env != NULL) {
            char path_copy[PATH_MAX];
            strncpy(path_copy, path_env, sizeof(path_copy) - 1);
            path_copy[sizeof(path_copy) - 1] = '\0';

            char *save = NULL;
            for (char *dir = strtok_r(path_copy, ":", &save); dir != NULL && match_count < 64;
                 dir = strtok_r(NULL, ":", &save)) {
                DIR *directory = opendir(dir);
            if (directory == NULL) {
                continue;
            }

            struct dirent *entry = NULL;
            while ((entry = readdir(directory)) != NULL && match_count < 64) {
                if (entry->d_name[0] == '.') {
                    continue;
                }
                if (strncmp(entry->d_name, token, token_len) != 0) {
                    continue;
                }

                int duplicate = 0;
                for (int i = 0; i < match_count; i++) {
                    if (strcmp(matches[i], entry->d_name) == 0) {
                        duplicate = 1;
                        break;
                    }
                }
                if (duplicate) {
                    continue;
                }

                char candidate[PATH_MAX];
                snprintf(candidate, sizeof(candidate), "%s/%s", dir, entry->d_name);
                if (access(candidate, X_OK) != 0) {
                    continue;
                }

                snprintf(matches[match_count], sizeof(matches[match_count]), "%s", entry->d_name);
                match_count++;
            }
            closedir(directory);
                 }
        }
    } else {
        char dirpart[PATH_MAX];
        const char *base = token;
        char *slash = strrchr(token, '/');
        if (slash != NULL) {
            size_t dir_len = (size_t)(slash - token);
            if (dir_len == 0) {
                strcpy(dirpart, "/");
            } else {
                memcpy(dirpart, token, dir_len);
                dirpart[dir_len] = '\0';
            }
            base = slash + 1;
        } else {
            if (!getcwd(dirpart, sizeof(dirpart))) {
                return;
            }
        }

        DIR *directory = opendir(dirpart);
        if (directory == NULL) {
            return;
        }

        size_t base_len = strlen(base);
        struct dirent *entry = NULL;
        while ((entry = readdir(directory)) != NULL && match_count < 64) {
            if (entry->d_name[0] == '.' && base_len == 0) {
                continue;
            }
            if (strncmp(entry->d_name, base, base_len) != 0) {
                continue;
            }

            char full[PATH_MAX];
            if (strcmp(dirpart, "/") == 0) {
                snprintf(full, sizeof(full), "/%s", entry->d_name);
            } else {
                snprintf(full, sizeof(full), "%s/%s", dirpart, entry->d_name);
            }

            struct stat st;
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
                strncat(full, "/", sizeof(full) - strlen(full) - 1);
            }

            if (slash != NULL) {
                char prefixed[PATH_MAX];
                size_t prefix_len = (size_t)(slash - token + 1);
                memcpy(prefixed, token, prefix_len);
                prefixed[prefix_len] = '\0';
                strncat(prefixed, full + (slash == token ? 1 : 0), sizeof(prefixed) - strlen(prefixed) - 1);
                snprintf(matches[match_count], sizeof(matches[match_count]), "%s", prefixed);
            } else {
                snprintf(matches[match_count], sizeof(matches[match_count]), "%s", full);
            }
            match_count++;
        }
        closedir(directory);
    }

    if (match_count == 0) {
        return;
    }

    if (match_count == 1) {
        const char *replacement = matches[0];
        size_t replace_len = strlen(replacement);
        if (*length + replace_len > token_len + (MAX_LINE - 1)) {
            return;
        }

        memmove(line + start + replace_len, line + end, *length - end + 1);
        memcpy(line + start, replacement, replace_len);
        *length = start + replace_len + (*length - end);
        *cursor = *length;
        redraw_line(line, *cursor);
        return;
    }

    int prefix = (int)strlen(matches[0]);
    for (int i = 1; i < match_count; i++) {
        prefix = common_prefix_len(matches[0], matches[i]);
        if (prefix <= (int)token_len) {
            prefix = (int)token_len;
            break;
        }
    }

    if (prefix > (int)token_len) {
        size_t add = (size_t)prefix - token_len;
        if (*length + add >= MAX_LINE - 1) {
            return;
        }
        memmove(line + start + prefix, line + end, *length - end + 1);
        memcpy(line + start, matches[0], (size_t)prefix);
        *length = start + prefix + (*length - end);
        *cursor = *length;
        redraw_line(line, *cursor);
    }

    putchar('\n');
    for (int i = 0; i < match_count; i++) {
        puts(matches[i]);
    }
    redraw_line(line, *cursor);
}

static int read_line(char *line, size_t size)
{
    if (!isatty(STDIN_FILENO)) {
        if (fgets(line, (int)size, stdin) == NULL) {
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

    for (;;) {
        int key = read_byte();
        if (key == -1) {
            terminal_restore();
            return -1;
        }

        if (key == '\n' || key == '\r') {
            putchar('\n');
            line[length] = '\0';
            terminal_restore();
            return 0;
        }

        if (key == 4) {
            terminal_restore();
            raise(SIGINT);
            return -1;
        }

        if (key == 127 || key == 8) {
            if (cursor > 0) {
                memmove(line + cursor - 1, line + cursor, length - cursor + 1);
                length--;
                cursor--;
                redraw_line(line, cursor);
            }
            continue;
        }

        if (key == '\t') {
            complete_token(line, &cursor, &length);
            continue;
        }

        if (key == 27) {
            key = read_escape_sequence();
            if (key == 256 && history_count > 0) {
                if (history_index > 0) {
                    history_index--;
                }
                int index = (history_start + history_index) % MAX_HISTORY;
                strncpy(line, history[index], size - 1);
                line[size - 1] = '\0';
                length = strlen(line);
                cursor = length;
                redraw_line(line, cursor);
            } else if (key == 257 && history_index < history_count) {
                history_index++;
                if (history_index == history_count) {
                    line[0] = '\0';
                } else {
                    int index = (history_start + history_index) % MAX_HISTORY;
                    strncpy(line, history[index], size - 1);
                    line[size - 1] = '\0';
                }
                length = strlen(line);
                cursor = length;
                redraw_line(line, cursor);
            } else if (key == 258 && cursor < length) {
                cursor++;
                redraw_line(line, cursor);
            } else if (key == 259 && cursor > 0) {
                cursor--;
                redraw_line(line, cursor);
            }
            continue;
        }

        if (key >= 32 && length + 1 < size - 1) {
            memmove(line + cursor + 1, line + cursor, length - cursor + 1);
            line[cursor] = (char)key;
            length++;
            cursor++;
            line[length] = '\0';
            redraw_line(line, cursor);
        }
    }
}

static int spawn_external(char **argv)
{
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid");
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static void print_help(void)
{
    puts("Built-in commands:");
    puts("  cd [dir]  - Change directory (default: HOME)");
    puts("  exit      - Exit the shell");
    puts("  help      - Display this help message");
    puts("External commands are also supported via $PATH.");
}

static int run_command(int argc, char **argv)
{
    if (argc == 0) {
        return 0;
    }

    if (strcmp(argv[0], "exit") == 0) {
        exit(0);
    }

    if (strcmp(argv[0], "help") == 0) {
        print_help();
        return 0;
    }

    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = argv[1] != NULL ? argv[1] : getenv("HOME");
        if (dir == NULL || chdir(dir) != 0) {
            perror("cd");
        }
        return 0;
    }

    return spawn_external(argv);
}

int main(void)
{
    signal(SIGINT, SIG_IGN);
    puts("Kyronix shell");
    puts("Type 'help' for commands.");

    history_load();

    char line[MAX_LINE];
    char *argv[MAX_ARGS];

    for (;;) {
        if (read_line(line, sizeof(line)) != 0) {
            putchar('\n');
            break;
        }

        int argc = split_line(line, argv);
        if (argc > 0) {
            history_add(line);
        }
        run_command(argc, argv);
    }

    terminal_restore();
    return 0;
}
