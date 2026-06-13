#include "libbb.h"

struct globals;
struct globals* ptr_to_globals;

void tcsetattr_stdin_TCSANOW(const struct termios* t)
{
    tcsetattr(STDIN_FILENO, TCSANOW, t);
}

void fflush_all(void)
{
    fflush(NULL);
}

ssize_t full_read(int fd, void* buf, size_t len)
{
    ssize_t total = 0;
    while (len > 0) {
        ssize_t n = read(fd, (char*) buf + total, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return total ? total : -1;
        }
        if (n == 0)
            return total;
        total += n;
        len -= n;
    }
    return total;
}

ssize_t full_write(int fd, const void* buf, size_t len)
{
    ssize_t total = 0;
    while (len > 0) {
        ssize_t n = write(fd, (const char*) buf + total, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return total ? total : -1;
        }
        total += n;
        len -= n;
    }
    return total;
}

int safe_poll(struct pollfd* fds, nfds_t nfds, int timeout)
{
    int n;
    do {
        n = poll(fds, nfds, timeout);
    } while (n < 0 && errno == EINTR);
    return n;
}

int get_terminal_width_height(int fd, unsigned* cols, unsigned* rows)
{
    struct winsize win;
    if (ioctl(fd, TIOCGWINSZ, &win) == 0) {
        *cols = win.ws_col;
        *rows = win.ws_row;
        return 0;
    }
    return -1;
}

int32_t read_key(int fd, char* buf UNUSED_PARAM, int timeout)
{
    unsigned char c;
    ssize_t n;

    if (timeout >= 0) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (safe_poll(&pfd, 1, timeout) <= 0)
            return -1;
    }

    n = read(fd, &c, 1);
    if (n <= 0)
        return -1;

    if (c == 0x1b) {
        unsigned char seq[8];
        int i;

        for (i = 0; i < 6; i++) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            if (poll(&pfd, 1, 20) <= 0)
                break;
            if (read(fd, &seq[i], 1) <= 0)
                break;
        }

        if (i == 0)
            return 0x1b;

        if (seq[0] == '[') {
            if (i >= 2) {
                switch (seq[1]) {
                case 'A':
                    return KEYCODE_UP;
                case 'B':
                    return KEYCODE_DOWN;
                case 'C':
                    return KEYCODE_RIGHT;
                case 'D':
                    return KEYCODE_LEFT;
                case 'H':
                    return KEYCODE_HOME;
                case 'F':
                    return KEYCODE_END;
                case '~':
                    switch (seq[2]) {
                    case '1':
                    case '7':
                        return KEYCODE_HOME;
                    case '2':
                        return KEYCODE_INSERT;
                    case '3':
                        return KEYCODE_DELETE;
                    case '4':
                    case '8':
                        return KEYCODE_END;
                    case '5':
                        return KEYCODE_PAGEUP;
                    case '6':
                        return KEYCODE_PAGEDOWN;
                    }
                    break;
                case '1':
                    if (i >= 3 && seq[2] == '5')
                        return KEYCODE_FUN5;
                    if (i >= 3 && seq[2] == '7')
                        return KEYCODE_FUN6;
                    if (i >= 3 && seq[2] == '8')
                        return KEYCODE_FUN7;
                    if (i >= 3 && seq[2] == '9')
                        return KEYCODE_FUN8;
                    break;
                case '2':
                    if (i >= 3 && seq[2] == '0')
                        return KEYCODE_FUN9;
                    if (i >= 3 && seq[2] == '1')
                        return KEYCODE_FUN10;
                    if (i >= 3 && seq[2] == '3')
                        return KEYCODE_FUN11;
                    if (i >= 3 && seq[2] == '4')
                        return KEYCODE_FUN12;
                    break;
                }
            }
        } else if (seq[0] == 'O') {
            if (i >= 1) {
                switch (seq[1]) {
                case 'H':
                    return KEYCODE_HOME;
                case 'F':
                    return KEYCODE_END;
                case 'P':
                    return KEYCODE_FUN1;
                case 'Q':
                    return KEYCODE_FUN2;
                case 'R':
                    return KEYCODE_FUN3;
                case 'S':
                    return KEYCODE_FUN4;
                }
            }
        }

        return 0x1b;
    }

    return c;
}

int vi_main(int, char**);
int main(int argc, char** argv)
{
    return vi_main(argc, argv);
}
