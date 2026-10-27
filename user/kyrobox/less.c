#include "common.h"
#include <stdarg.h>
#include <sys/ioctl.h>
#include <termios.h>

#define TAB_WIDTH 8

static struct {
    char **data;
    size_t cap;
    size_t count;
} lines;

static size_t top;
static size_t cur;
static int cols;
static int rows;
static bool raw_active;
static struct termios orig_tio;
static bool has_file;
static const char *fname;
static long total_bytes;
static int max_lineno;

static void add_line(const char *s) {
    if (lines.count >= lines.cap) {
        lines.cap = lines.cap ? lines.cap * 2 : 4096;
        lines.data = realloc(lines.data, lines.cap * sizeof(char *));
        if (!lines.data) kx_die("Out of memory");
    }
    lines.data[lines.count] = strdup(s);
    if (!lines.data[lines.count]) kx_die("Out of memory");
    lines.count++;
}

static void read_stdin(void) {
    char *buf = NULL;
    size_t bufsz = 0;
    while (getline(&buf, &bufsz, stdin) > 0) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = 0;
        add_line(buf);
    }
    free(buf);
}

static void read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        kx_warn(path);
        exit(1);
    }
    fname = path;
    char *buf = NULL;
    size_t bufsz = 0;
    while (getline(&buf, &bufsz, f) > 0) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = 0;
        total_bytes += (long) len;
        add_line(buf);
    }
    free(buf);
    fclose(f);
}

static void restore_term(void) {
    if (!raw_active) return;
    raw_active = false;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_tio);
    fputs("\033[?25h\033[0m", stderr);
}

static void set_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &orig_tio);
    struct termios raw = orig_tio;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_active = true;
    fputs("\033[?25l", stderr);
}

static int get_win_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
        return 0;
    }
    cols = 80;
    rows = 24;
    return -1;
}

static volatile int sigwinch_flag;

static void sigwinch_handler(int sig) {
    (void) sig;
    sigwinch_flag = 1;
}

static void print_line(const char *s, size_t lineno, bool highlight) {
    if (highlight) fputs("\033[7m", stdout);
    int w = max_lineno > 0 ? snprintf(NULL, 0, "%zu", lineno) : 0;
    if (max_lineno > 0) printf(" %*zu ", w, lineno + 1);
    size_t vpos = max_lineno > 0 ? (size_t) w + 3 : 0;
    for (; *s && vpos < (size_t) cols; s++) {
        unsigned char c = (unsigned char) *s;
        if (c == '\t') {
            int stop = TAB_WIDTH - (vpos % TAB_WIDTH);
            if (vpos + stop > (size_t) cols) stop = (int) ((size_t) cols - vpos);
            fputs("        " + (TAB_WIDTH - stop), stdout);
            vpos += stop;
        } else if (c < 32) {
            if (vpos + 2 > (size_t) cols) break;
            putchar('^');
            putchar('@' + c);
            vpos += 2;
        } else if (c == 127) {
            if (vpos + 2 > (size_t) cols) break;
            fputs("^?", stdout);
            vpos += 2;
        } else {
            putchar(c);
            vpos++;
        }
    }
    if (highlight) fputs("\033[27m", stdout);
    putchar('\n');
}

static void clear_screen(void) { fputs("\033[H\033[J", stdout); }

static void move_to(int r, int c) { printf("\033[%d;%dH", r + 1, c + 1); }

static void status_line(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs("\033[7m", stdout);
    move_to(rows - 1, 0);
    fputs("\033[K", stdout);
    size_t slen = strlen(buf);
    if (slen > (size_t) cols) buf[cols] = 0;
    fputs(buf, stdout);
    fputs("\033[27m", stdout);
}

static void update_status(void) {
    if (lines.count == 0) {
        status_line("%s (END)", fname ? fname : "[no file]");
        return;
    }
    size_t page = (size_t) rows - 1;
    size_t total = lines.count;
    size_t bot = top + page;
    if (bot > total) bot = total;
    size_t pct = (top + 1) * 100 / total;
    if (pct > 100) pct = 100;
    const char *fn = fname ? fname : "[stdin]";
    if (bot >= total)
        status_line("%s (END) %%, line %zu/%zu", fn, bot, total);
    else
        status_line("%s %zu%% (line %zu-%zu/%zu)", fn, pct, top + 1, bot, total);
}

static void display(void) {
    clear_screen();
    int page = rows - 1;
    size_t end = top + page;
    if (end > lines.count) end = lines.count;
    for (size_t i = top; i < end; i++) {
        bool hl = (i == cur && cur >= top && cur < end);
        print_line(lines.data[i], i, hl);
    }
    size_t shown = end - top;
    for (size_t i = shown; i < (size_t) page; i++) fputs("~\n", stdout);
    update_status();
    move_to(cur - top, 0);
}

static void scroll_to(size_t t) {
    if (t >= lines.count) t = lines.count > 0 ? lines.count - 1 : 0;
    cur = t;
    int page = rows - 1;
    if (lines.count <= (size_t) page) {
        top = 0;
    } else if (cur < top) {
        top = cur;
    } else if (cur >= top + page) {
        top = cur - page + 1;
        if (top + page > lines.count) top = lines.count - page;
    }
    display();
}

static char read_key(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return 0;
    return (char) c;
}

static int read_escape_seq(void) {
    char c = read_key();
    if (c == '[') {
        c = read_key();
        if (c >= '0' && c <= '9') {
            int num = c - '0';
            c = read_key();
            while (c >= '0' && c <= '9') {
                num = num * 10 + (c - '0');
                c = read_key();
            }
            if (c == '~') return 1000 + num;
            return 0;
        }
        switch (c) {
        case 'A':
            return -1; /* up */
        case 'B':
            return -2; /* down */
        case 'C':
            return -3; /* right */
        case 'D':
            return -4; /* left */
        case 'H':
            return -5; /* home */
        case 'F':
            return -6; /* end */
        default:
            return 0;
        }
    }
    if (c == 'O') {
        c = read_key();
        switch (c) {
        case 'H':
            return -5; /* home */
        case 'F':
            return -6; /* end */
        default:
            return 0;
        }
    }
    return 0;
}

static int read_key_press(void) {
    char c = read_key();
    if (c == '\033') {
        int seq = read_escape_seq();
        if (seq) return seq;
        return '\033';
    }
    return (unsigned char) c;
}

static size_t find_next(const char *pat, size_t start) {
    for (size_t i = start; i < lines.count; i++)
        if (strstr(lines.data[i], pat)) return i;
    return lines.count;
}

static size_t find_prev(const char *pat, size_t start) {
    for (size_t i = start; i > 0; i--)
        if (strstr(lines.data[i - 1], pat)) return i - 1;
    return lines.count;
}

static int search_prompt(const char *prompt) {
    fputs("\033[7m", stderr);
    move_to(rows - 1, 0);
    fputs("\033[K", stderr);
    fprintf(stderr, "%s", prompt);
    fputs("\033[27m", stderr);
    char buf[256];
    size_t pos = 0;
    for (;;) {
        move_to(rows - 1, (int) strlen(prompt) + (int) pos);
        int c = read_key_press();
        if (c == '\n' || c == '\r') {
            buf[pos] = 0;
            break;
        }
        if ((c == 127 || c == 8) && pos > 0) {
            pos--;
            fputs("\b \b", stderr);
            continue;
        }
        if (c == 27 || c == 3) {
            buf[0] = 0;
            break;
        }
        if (pos < sizeof(buf) - 1 && c >= 32 && c < 127) {
            buf[pos++] = (char) c;
            putc(c, stderr);
        }
    }
    if (buf[0] == 0) return 0;
    size_t found;
    if (prompt[0] == '/') {
        found = find_next(buf, cur + 1);
        if (found >= lines.count) found = find_next(buf, 0);
    } else {
        found = find_prev(buf, cur);
        if (found >= lines.count) found = find_prev(buf, lines.count);
    }
    if (found < lines.count) {
        scroll_to(found);
        return 1;
    }
    status_line("Pattern not found: %s", buf);
    return 0;
}

static void usage(void) {
    printf("usage: %s [file...]\n", kx_prog);
    exit(0);
}

int main(int argc, char **argv) {
    kx_prog = "less";
    if (argc > 1 && strcmp(argv[1], "--help") == 0) usage();
    has_file = argc > 1;
    if (has_file) {
        for (int i = 1; i < argc; i++) read_file(argv[i]);
    } else {
        if (isatty(STDIN_FILENO)) {
            fprintf(stderr, "%s: Missing filename\n", kx_prog);
            exit(1);
        }
        read_stdin();
    }
    if (lines.count == 0) return 0;
    max_lineno = (int) snprintf(NULL, 0, "%zu", lines.count);
    if (!isatty(STDOUT_FILENO)) {
        for (size_t i = 0; i < lines.count; i++) puts(lines.data[i]);
        for (size_t i = 0; i < lines.count; i++) free(lines.data[i]);
        free(lines.data);
        return 0;
    }
    set_raw_mode();
    get_win_size();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigwinch_handler;
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, NULL);
    scroll_to(0);
    for (;;) {
        if (sigwinch_flag) {
            sigwinch_flag = 0;
            get_win_size();
            display();
        }
        int c = read_key_press();
        int page = rows - 1;
        switch (c) {
        case 'q':
        case 'Q':
            restore_term();
            for (size_t i = 0; i < lines.count; i++) free(lines.data[i]);
            free(lines.data);
            return 0;
        case 'j':
        case -2:
            if (cur + 1 < lines.count) scroll_to(cur + 1);
            break;
        case 'k':
        case -1:
            if (cur > 0) scroll_to(cur - 1);
            break;
        case ' ':
        case 'f':
        case -6:
            scroll_to(cur + page);
            break;
        case 'b':
            if (cur > (size_t) page)
                scroll_to(cur - page);
            else
                scroll_to(0);
            break;
        case 'd':
            scroll_to(cur + page / 2);
            break;
        case 'u':
            if (cur > (size_t) (page / 2))
                scroll_to(cur - page / 2);
            else
                scroll_to(0);
            break;
        case 'g':
        case -5:
            scroll_to(0);
            break;
        case 'G':
            if (lines.count > 0) scroll_to(lines.count - 1);
            break;
        case '/':
            search_prompt("/");
            break;
        case '?':
            search_prompt("?");
            break;
        case '=': {
            size_t pct = lines.count > 0 ? (cur + 1) * 100 / lines.count : 0;
            if (pct > 100) pct = 100;
            status_line("line %zu/%zu  %zu%%", cur + 1, lines.count, pct);
            break;
        }
        default:
            break;
        }
    }
}
