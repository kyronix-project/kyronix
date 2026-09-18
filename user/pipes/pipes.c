#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define VERSION "1.3.0"
#define RAND_M 32768
#define MAX_PIPES 256
#define MAX_SETS 64

static const char *default_sets[] = {
    "┃┏ ┓┛━┓  ┗┃┛┗ ┏━",     "│╭ ╮╯─╮  ╰│╯╰ ╭─",     "│┌ ┐┘─┐  └│┘└ ┌─", "║╔ ╗╝═╗  ╚║╝╚ ╔═",
    "|+ ++-+  +|++ +-",     "|/ \\/\\-  \\|/\\ /-", ".. ....  .... ..", ".o oo.o  o.oo o.",
    "-\\ /\\|/  /-\\/ \\|", "╿┍ ┑┚╼┒  ┕╽┙┖ ┎╾",
};

static int nsets;
static const char *sets[MAX_SETS];

static int types[MAX_SETS];
static int ntypes;
static int colors[MAX_PIPES];
static int ncolors_sel;

static int npipes = 1;
static int fps = 75;
static int straight = 13;
static int reset_n = 2000;
static int rnd_start;
static int opt_bold = 1;
static int opt_nocolor;
static int opt_keepct;

static int cols = 80, rows = 24;

static int px[MAX_PIPES], py[MAX_PIPES];
static int pdir[MAX_PIPES];
static int ptype[MAX_PIPES];
static int pcolor[MAX_PIPES];

static char eseqs[MAX_PIPES][32];

static int iter;

static volatile sig_atomic_t sig_resize;
static volatile sig_atomic_t sig_exit;

static int utf8_off(const char *s, int n) {
    int off = 0;
    while (n-- > 0) {
        off++;
        while ((s[off] & 0xC0) == 0x80) off++;
    }
    return off;
}

static int utf8_len(const char *s) {
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static int utf8_strlen(const char *s) {
    int n = 0;
    while (*s) {
        if ((*s & 0xC0) != 0x80) n++;
        s++;
    }
    return n;
}

static int rnd(int n) { return rand() % n; }

static void sigh(int sig) {
    if (sig == SIGWINCH)
        sig_resize = 1;
    else
        sig_exit = 1;
}

static struct termios saved_tio;
static int tio_saved;

static void tty_raw(void) {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &saved_tio) < 0) return;
    struct termios raw = saved_tio;
    raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    tio_saved = 1;
}

static void tty_restore(void) {
    if (tio_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tio);
        tio_saved = 0;
    }
}

static void get_termsize(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
}

static void print_help(const char *prog) {
    printf("Usage: %s [OPTION]...\n"
           "Animated pipes terminal screensaver.\n"
           "\n"
           "  -p [1-]               number of pipes (D=1)\n"
           "  -t [0-%d]              pipe type (D=0)\n"
           "  -t c[16 chars]        custom pipe type\n"
           "  -c [0-7]              pipe color INDEX (D=-c 1 -c 2 ... -c 7 -c 0)\n"
           "  -f [20-100]           framerate (D=75)\n"
           "  -s [5-15]             going straight probability, 1 in (D=13)\n"
           "  -r [0-]               reset after (D=2000) chars, 0 = no reset\n"
           "  -R                    randomize starting position and direction\n"
           "  -B                    no bold effect\n"
           "  -C                    no color\n"
           "  -K                    keep pipe color and type when crossing edges\n"
           "  -h                    print this help message\n"
           "  -v                    print version number\n"
           "  -t and -c can be used more than once.\n",
           prog, nsets - 1);
}

static void build_eseqs(void) {
    for (int i = 0; i < ncolors_sel; i++) {
        char *e = eseqs[i];
        int pos = 0;
        e[pos++] = '\033';
        e[pos++] = '[';
        e[pos++] = 'm';
        if (opt_bold) {
            memcpy(e + pos, "\033[1m", 4);
            pos += 4;
        }
        if (!opt_nocolor) { pos += sprintf(e + pos, "\033[%dm", 30 + colors[i]); }
        e[pos] = '\0';
    }
}

static void init_pipes(void) {
    int ci = 0, vi = 0;
    for (int i = 0; i < npipes; i++) {
        px[i] = rnd_start ? rnd(cols) : cols / 2;
        py[i] = rnd_start ? rnd(rows) : rows / 2;
        pdir[i] = rnd_start ? rnd(4) : 0;
        ptype[i] = types[vi];
        pcolor[i] = ci;
        ci = (ci + 1) % ncolors_sel;
        vi = (vi + 1) % ntypes;
    }
}

static void rebuild_eseqs(void) {
    build_eseqs();
    for (int i = 0; i < npipes; i++) pcolor[i] = pcolor[i] % ncolors_sel;
}

static void finish(void) {
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    char dump[64];
    while (poll(&pfd, 1, 0) > 0) {
        if (read(STDIN_FILENO, dump, sizeof(dump)) <= 0) break;
    }
    write(STDOUT_FILENO, "\033[?1049l\033[?25h\033[m", 18);
    tty_restore();
    _exit(0);
}

int main(int argc, char **argv) {
    if (!isatty(STDOUT_FILENO)) {
        fprintf(stderr, "stdout is not a terminal\n");
        return 1;
    }

    srand((unsigned) time(NULL) ^ (unsigned) getpid());

    nsets = sizeof(default_sets) / sizeof(default_sets[0]);
    for (int i = 0; i < nsets; i++) sets[i] = default_sets[i];

    int opt;
    while ((opt = getopt(argc, argv, "p:t:c:f:s:r:RBCKhv")) != -1) {
        switch (opt) {
        case 'p': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v <= 0 || v > MAX_PIPES) {
                fprintf(stderr,
                        "%s: -p invalid argument -- %s; "
                        "must be an integer 1-%d\n",
                        argv[0], optarg, MAX_PIPES);
                return 1;
            }
            npipes = (int) v;
            break;
        }
        case 't': {
            if (optarg[0] == 'c' && utf8_strlen(optarg + 1) == 16) {
                if (nsets >= MAX_SETS) {
                    fprintf(stderr, "%s: too many custom types\n", argv[0]);
                    return 1;
                }
                sets[nsets] = strdup(optarg + 1);
                types[ntypes++] = nsets;
                nsets++;
            } else {
                char *end;
                long v = strtol(optarg, &end, 10);
                if (*end || v < 0 || v >= nsets) {
                    fprintf(stderr,
                            "%s: -t invalid argument -- %s; "
                            "must be 0-%d or c[16 chars]\n",
                            argv[0], optarg, nsets - 1);
                    return 1;
                }
                types[ntypes++] = (int) v;
            }
            break;
        }
        case 'c': {
            char *end;
            long v;
            if (optarg[0] == '#') {
                v = strtol(optarg + 1, &end, 16);
                if (*end || v < 0 || v >= 8) {
                    fprintf(stderr,
                            "%s: -c invalid hex -- %s; "
                            "must be #0-#7\n",
                            argv[0], optarg);
                    return 1;
                }
            } else {
                v = strtol(optarg, &end, 10);
                if (*end || v < 0 || v >= 8) {
                    fprintf(stderr,
                            "%s: -c invalid argument -- %s; "
                            "must be 0-7 or #hex\n",
                            argv[0], optarg);
                    return 1;
                }
            }
            colors[ncolors_sel++] = (int) v;
            break;
        }
        case 'f': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 20 || v > 100) {
                fprintf(stderr,
                        "%s: -f invalid argument -- %s; "
                        "must be 20-100\n",
                        argv[0], optarg);
                return 1;
            }
            fps = (int) v;
            break;
        }
        case 's': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 5 || v > 15) {
                fprintf(stderr,
                        "%s: -s invalid argument -- %s; "
                        "must be 5-15\n",
                        argv[0], optarg);
                return 1;
            }
            straight = (int) v;
            break;
        }
        case 'r': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 0) {
                fprintf(stderr,
                        "%s: -r invalid argument -- %s; "
                        "must be >= 0\n",
                        argv[0], optarg);
                return 1;
            }
            reset_n = (int) v;
            break;
        }
        case 'R':
            rnd_start = 1;
            break;
        case 'B':
            opt_bold = 0;
            break;
        case 'C':
            opt_nocolor = 1;
            break;
        case 'K':
            opt_keepct = 1;
            break;
        case 'h':
            print_help(argv[0]);
            return 0;
        case 'v':
            printf("%s %s\n", argv[0], VERSION);
            return 0;
        default:
            return 1;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "%s: illegal arguments -- ", argv[0]);
        for (int i = optind; i < argc; i++) fprintf(stderr, "%s ", argv[i]);
        fputc('\n', stderr);
        return 1;
    }

    if (ntypes == 0) { types[ntypes++] = 0; }
    if (ncolors_sel == 0) {
        int def[] = { 1, 2, 3, 4, 5, 6, 7, 0 };
        for (int i = 0; i < 8; i++) colors[i] = def[i];
        ncolors_sel = 8;
    }

    get_termsize();
    build_eseqs();

    signal(SIGTERM, sigh);
    signal(SIGHUP, sigh);
    signal(SIGINT, sigh);
    signal(SIGWINCH, sigh);

    write(STDOUT_FILENO, "\033[?1049h\033[?25l\033[2J\033[H", 19);
    tty_raw();

    init_pipes();

    char key;
    for (;;) {
        if (sig_exit) finish();
        if (sig_resize) {
            get_termsize();
            sig_resize = 0;
        }

        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
        int to = (fps > 0) ? (1000 / fps) : 13;
        if (poll(&pfd, 1, to) > 0) {
            if (read(STDIN_FILENO, &key, 1) == 1) {
                switch (key) {
                case 'P':
                    if (straight < 15) straight++;
                    break;
                case 'O':
                    if (straight > 3) straight--;
                    break;
                case 'F':
                    if (fps < 100) fps++;
                    break;
                case 'D':
                    if (fps > 20) fps--;
                    break;
                case 'B':
                    opt_bold = !opt_bold;
                    rebuild_eseqs();
                    break;
                case 'C':
                    opt_nocolor = !opt_nocolor;
                    rebuild_eseqs();
                    break;
                case 'K':
                    opt_keepct = !opt_keepct;
                    break;
                default:
                    finish();
                    break;
                }
            }
        }

        for (int i = 0; i < npipes; i++) {
            if (pdir[i] & 1)
                px[i] += -pdir[i] + 2;
            else
                py[i] += pdir[i] - 1;

            if (!opt_keepct && (px[i] >= cols || px[i] < 0 || py[i] >= rows || py[i] < 0)) {
                pcolor[i] = rnd(ncolors_sel);
                ptype[i] = types[rnd(ntypes)];
            }
            px[i] = (px[i] + cols) % cols;
            py[i] = (py[i] + rows) % rows;

            int nd = straight * rnd(RAND_M) / RAND_M - 1;
            if (nd >= 0)
                nd = pdir[i];
            else
                nd = pdir[i] + (2 * rnd(2) - 1);
            nd = (nd + 4) % 4;

            int off = utf8_off(sets[ptype[i]], pdir[i] * 4 + nd);
            int len = utf8_len(sets[ptype[i]] + off);
            dprintf(STDOUT_FILENO, "\033[%d;%dH%s%.*s", py[i] + 1, px[i] + 1, eseqs[pcolor[i]], len,
                    sets[ptype[i]] + off);

            pdir[i] = nd;
        }

        if (reset_n > 0 && ++iter * npipes >= reset_n) {
            write(STDOUT_FILENO, "\033[2J\033[H\033[?25l", 12);
            iter = 0;
        }
    }

    finish();
    return 0;
}
