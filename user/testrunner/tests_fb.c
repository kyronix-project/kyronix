/*
 * A TUI is a byte protocol, so these tests talk the protocol and read the
 * result back out of video memory through /dev/fb0. That is the only honest
 * way to check the console: nvim sends smcup, IL/DL, DCH/ICH and a cursor
 * position query without ever asking whether any of it was understood, and a
 * terminal that silently drops them produces a blank screen rather than an
 * error.
 *
 * Every test restores the primary screen and the default scroll region, since
 * the framebuffer is kernel state shared with the rest of the suite.
 */
#include "test_harness.h"

#define FBIOGET_VSCREENINFO 0x4600

typedef struct {
    uint32_t xres, yres;
    uint32_t xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    uint32_t r_off, r_len, r_msb, g_off, g_len, g_msb;
    uint32_t b_off, b_len, b_msb, t_off, t_len, t_msb;
    uint32_t nonstd, activate, height, width, accel_flags, pixclock;
    uint32_t left, right, upper, lower, hsync, vsync, sync, vmode, rotate;
    uint32_t colorspace, reserved[4];
} fb_var_t;

typedef struct {
    int tty;
    int fb;
    unsigned char *pixels;
    size_t bytes;
    int pitch;
    int cols;
    int rows;
} fbscreen_t;

static int fb_screen_open(fbscreen_t *s) {
    memset(s, 0, sizeof(*s));
    s->tty = open("/dev/tty", O_RDWR);
    if (s->tty < 0) return 0;
    s->fb = open("/dev/fb0", O_RDONLY);
    if (s->fb < 0) {
        close(s->tty);
        return 0;
    }

    fb_var_t vi;
    memset(&vi, 0, sizeof(vi));
    if (ioctl(s->fb, FBIOGET_VSCREENINFO, &vi) != 0 || vi.bits_per_pixel != 32) {
        close(s->fb);
        close(s->tty);
        return 0;
    }
    s->pitch = (int) vi.xres * 4;
    s->bytes = (size_t) s->pitch * vi.yres;
    s->cols = (int) (vi.xres / 8);
    s->rows = (int) (vi.yres / 16);
    s->pixels = malloc(s->bytes);
    if (!s->pixels) {
        close(s->fb);
        close(s->tty);
        return 0;
    }
    return 1;
}

static void fb_screen_close(fbscreen_t *s) {
    free(s->pixels);
    if (s->fb >= 0) close(s->fb);
    if (s->tty >= 0) close(s->tty);
}

/* Put the console back the way the rest of the suite expects it. The scroll
 * region in particular outlives a test and would make later output scroll
 * inside a window that no longer matches the visible screen. */
static void fb_restore(fbscreen_t *s) {
    static const char reset[] = "\033[?1049l\033[?47l\033[r\033[0m\033[2J\033[H";
    ssize_t r = write(s->tty, reset, sizeof(reset) - 1);
    (void) r;
}

static int fb_send(fbscreen_t *s, const char *seq) {
    size_t len = strlen(seq);
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(s->tty, seq + done, len - done);
        if (w <= 0) return 0;
        done += (size_t) w;
    }
    return 1;
}

/* /dev/fb0 is a character device, so it has no seekable position: the kernel
 * rejects pread and lseek on it. A fresh open starts at offset 0 and one read
 * of the whole buffer gets the entire frame. */
static int fb_snapshot(fbscreen_t *s) {
    int fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0) return 0;
    size_t done = 0;
    while (done < s->bytes) {
        ssize_t r = read(fd, s->pixels + done, s->bytes - done);
        if (r <= 0) break;
        done += (size_t) r;
    }
    close(fd);
    return done == s->bytes;
}

static uint32_t fb_cell_px(const fbscreen_t *s, int col, int row, int fx, int fy) {
    int x = col * 8 + fx;
    int y = row * 16 + fy;
    const uint32_t *p = (const uint32_t *) (s->pixels + (size_t) y * s->pitch + (size_t) x * 4);
    return *p;
}

/* Ink = pixels that differ from the cell's own background. Counting it says
 * "something was drawn here" without depending on the exact glyph shape. */
static int fb_cell_ink(const fbscreen_t *s, int col, int row) {
    uint32_t bg = fb_cell_px(s, col, row, 1, 1);
    int ink = 0;
    for (int fy = 0; fy < 16; fy++)
        for (int fx = 0; fx < 8; fx++)
            if (fb_cell_px(s, col, row, fx, fy) != bg) ink++;
    return ink;
}

/* Ask the terminal where the cursor is. The answer arrives as bytes on the
 * terminal's own input, which is exactly the path an application reads. */
static int fb_query_cursor(fbscreen_t *s, int *row, int *col) {
    if (!fb_send(s, "\033[6n")) return 0;
    char buf[64];
    size_t n = 0;
    for (int spin = 0; spin < 200 && n < sizeof(buf) - 1; spin++) {
        ssize_t r = read(s->tty, buf + n, 1);
        if (r == 1) {
            if (buf[n] == 'R') {
                n++;
                break;
            }
            n++;
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        break;
    }
    buf[n] = '\0';
    /* ESC [ rows ; cols R */
    if (n < 6 || buf[0] != '\033' || buf[1] != '[') return 0;
    int r = 0, c = 0;
    size_t i = 2;
    if (i >= n || buf[i] < '0' || buf[i] > '9') return 0;
    while (i < n && buf[i] >= '0' && buf[i] <= '9') r = r * 10 + (buf[i++] - '0');
    if (i >= n || buf[i] != ';') return 0;
    i++;
    if (i >= n || buf[i] < '0' || buf[i] > '9') return 0;
    while (i < n && buf[i] >= '0' && buf[i] <= '9') c = c * 10 + (buf[i++] - '0');
    if (i >= n || buf[i] != 'R') return 0;
    *row = r;
    *col = c;
    return 1;
}

static int fb_ready(fbscreen_t *s) {
    if (!fb_screen_open(s)) return 0;
    if (s->cols < 20 || s->rows < 4) {
        fb_screen_close(s);
        return 0;
    }
    fb_restore(s);
    return 1;
}

int test_fb_dev_fb0_present(void) {
    fbscreen_t s;
    ASSERT_TRUE(fb_ready(&s));
    ASSERT_GT(s.cols, 0);
    ASSERT_GT(s.rows, 0);
    fb_screen_close(&s);
    return 1;
}
REGISTER_TEST(fb_dev_fb0_present, "Drivers");

int test_fb_alternate_screen(void) {
    fbscreen_t s;
    ASSERT_TRUE(fb_ready(&s));

    /* Something visible on the primary screen, at the home cell. */
    ASSERT_TRUE(fb_send(&s, "\033[41m    \033[0m"));
    ASSERT_TRUE(fb_snapshot(&s));
    uint32_t primary = fb_cell_px(&s, 0, 0, 1, 1);

    /* ?1049 switches to a cleared screen 1 and parks the cursor at home. The
     * cell that was red on the primary screen is background here. */
    ASSERT_TRUE(fb_send(&s, "\033[?1049h"));
    ASSERT_TRUE(fb_snapshot(&s));
    ASSERT_NE((int) primary, (int) fb_cell_px(&s, 0, 0, 1, 1));

    /* And leaving puts the original contents back, cell for cell. */
    ASSERT_TRUE(fb_send(&s, "\033[?1049l"));
    ASSERT_TRUE(fb_snapshot(&s));
    ASSERT_EQ((int) primary, (int) fb_cell_px(&s, 0, 0, 1, 1));

    /* ?47 is the variant xterm_color's smcup asks for. It must also switch. */
    ASSERT_TRUE(fb_send(&s, "\033[?47h\033[42m    "));
    ASSERT_TRUE(fb_snapshot(&s));
    uint32_t alt47 = fb_cell_px(&s, 0, 0, 1, 1);
    ASSERT_NE((int) primary, (int) alt47);
    ASSERT_TRUE(fb_send(&s, "\033[?47l"));
    ASSERT_TRUE(fb_snapshot(&s));
    ASSERT_EQ((int) primary, (int) fb_cell_px(&s, 0, 0, 1, 1));

    fb_restore(&s);
    fb_screen_close(&s);
    return 1;
}
REGISTER_TEST(fb_alternate_screen, "Drivers");

int test_fb_decsc_decrc(void) {
    fbscreen_t s;
    ASSERT_TRUE(fb_ready(&s));

    ASSERT_TRUE(fb_send(&s, "\033[5;10H\0337\033[1;1H"));
    int row = 0, col = 0;
    ASSERT_TRUE(fb_query_cursor(&s, &row, &col));
    ASSERT_EQ(1, row);
    ASSERT_EQ(1, col);

    /* ESC 8 must bring back both the position saved by ESC 7 and the colours
     * that were in force when it was taken. */
    ASSERT_TRUE(fb_send(&s, "\0338"));
    ASSERT_TRUE(fb_query_cursor(&s, &row, &col));
    ASSERT_EQ(5, row);
    ASSERT_EQ(10, col);

    fb_restore(&s);
    fb_screen_close(&s);
    return 1;
}
REGISTER_TEST(fb_decsc_decrc, "Drivers");

int test_fb_dsr_reports_cursor(void) {
    fbscreen_t s;
    ASSERT_TRUE(fb_ready(&s));

    ASSERT_TRUE(fb_send(&s, "\033[3;7H"));
    int row = 0, col = 0;
    ASSERT_TRUE(fb_query_cursor(&s, &row, &col));
    ASSERT_EQ(3, row);
    ASSERT_EQ(7, col);

    /* A terminal that never answers 6n makes an application hang or draw at
     * the wrong place, so silence is the failure this catches. */
    ASSERT_TRUE(fb_send(&s, "\033[10;20H"));
    ASSERT_TRUE(fb_query_cursor(&s, &row, &col));
    ASSERT_EQ(10, row);
    ASSERT_EQ(20, col);

    fb_restore(&s);
    fb_screen_close(&s);
    return 1;
}
REGISTER_TEST(fb_dsr_reports_cursor, "Drivers");

int test_fb_da1_reply(void) {
    fbscreen_t s;
    ASSERT_TRUE(fb_ready(&s));

    /* DA1 answers with a device string; vim and nvim both probe this early. */
    ASSERT_TRUE(fb_send(&s, "\033[c"));
    char buf[32];
    size_t n = 0;
    for (int spin = 0; spin < 200 && n < sizeof(buf) - 1; spin++) {
        ssize_t r = read(s.tty, buf + n, 1);
        if (r == 1) {
            n++;
            if (buf[n - 1] == 'c') break;
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        break;
    }
    buf[n] = '\0';
    ASSERT_NOTNULL(strstr(buf, "\033[?1;2c"));

    fb_restore(&s);
    fb_screen_close(&s);
    return 1;
}
REGISTER_TEST(fb_da1_reply, "Drivers");

int test_fb_scroll_region_is_honoured(void) {
    fbscreen_t s;
    ASSERT_TRUE(fb_ready(&s));

    /* Mark row 1 and set a region that begins at row 5. The cursor is parked
     * on row 1, above the region. */
    ASSERT_TRUE(fb_send(&s, "\033[2J\033[H\033[7m\033[2;1HMARK"));
    ASSERT_TRUE(fb_snapshot(&s));
    int marked = fb_cell_ink(&s, 0, 1);
    ASSERT_GT(marked, 0);

    /* IL from outside the region must be refused outright. A terminal that
     * forgot the region treats the whole screen as scrollable, inserts at row
     * 1 and pushes the text down to row 2 -- so the cell loses its ink. */
    ASSERT_TRUE(fb_send(&s, "\033[5;10r\033[2;1H\033[L"));
    ASSERT_TRUE(fb_snapshot(&s));
    ASSERT_EQ(marked, fb_cell_ink(&s, 0, 1));

    /* Inside the region the same IL must work: row 5 blanks out. */
    ASSERT_TRUE(fb_send(&s, "\033[5;1H\033[7mINSIDE"));
    ASSERT_TRUE(fb_snapshot(&s));
    ASSERT_GT(fb_cell_ink(&s, 0, 4), 0);
    ASSERT_TRUE(fb_send(&s, "\033[5;1H\033[L"));
    ASSERT_TRUE(fb_snapshot(&s));
    ASSERT_EQ(0, fb_cell_ink(&s, 0, 4));
    ASSERT_EQ(marked, fb_cell_ink(&s, 0, 1));

    fb_restore(&s);
    fb_screen_close(&s);
    return 1;
}
REGISTER_TEST(fb_scroll_region_is_honoured, "Drivers");

int test_fb_insert_delete_lines(void) {
    fbscreen_t s;
    ASSERT_TRUE(fb_ready(&s));

    ASSERT_TRUE(fb_send(&s, "\033[2J\033[H\033[7mTOP"));
    ASSERT_TRUE(fb_snapshot(&s));
    ASSERT_GT(fb_cell_ink(&s, 0, 0), 0);

    /* IL at row 5 inserts a blank line there, shifting rows 5.. down. Row 0
     * sits above the insertion point and must be untouched. */
    ASSERT_TRUE(fb_send(&s, "\033[6;1H\033[L"));
    ASSERT_TRUE(fb_snapshot(&s));
    ASSERT_GT(fb_cell_ink(&s, 0, 0), 0);

    /* DL of the whole screen's worth pulls everything up and blanks the tail. */
    ASSERT_TRUE(fb_send(&s, "\033[1;1H\033[100M"));
    ASSERT_TRUE(fb_snapshot(&s));
    ASSERT_EQ(0, fb_cell_ink(&s, 0, 0));

    fb_restore(&s);
    fb_screen_close(&s);
    return 1;
}
REGISTER_TEST(fb_insert_delete_lines, "Drivers");

int test_fb_insert_delete_chars(void) {
    fbscreen_t s;
    ASSERT_TRUE(fb_ready(&s));

    ASSERT_TRUE(fb_send(&s, "\033[2J\033[H\033[7mABCDEFGH"));
    ASSERT_TRUE(fb_snapshot(&s));
    ASSERT_GT(fb_cell_ink(&s, 0, 0), 0);

    /* ICH 4 at column 0 pushes the text right by four cells: the eight glyphs
     * now occupy cells 4..11, so cell 9 that was blank carries ink. */
    ASSERT_TRUE(fb_send(&s, "\033[1;1H\033[4@"));
    ASSERT_TRUE(fb_snapshot(&s));
    ASSERT_EQ(0, fb_cell_ink(&s, 0, 0));
    ASSERT_EQ(0, fb_cell_ink(&s, 3, 0));
    ASSERT_GT(fb_cell_ink(&s, 4, 0), 0);
    ASSERT_GT(fb_cell_ink(&s, 9, 0), 0);

    /* DCH 4 pulls it back to the left margin: eight glyphs land in cells 0..7
     * and the four cells they vacated are blank. */
    ASSERT_TRUE(fb_send(&s, "\033[1;1H\033[4P"));
    ASSERT_TRUE(fb_snapshot(&s));
    ASSERT_GT(fb_cell_ink(&s, 0, 0), 0);
    ASSERT_GT(fb_cell_ink(&s, 7, 0), 0);
    ASSERT_EQ(0, fb_cell_ink(&s, 9, 0));

    fb_restore(&s);
    fb_screen_close(&s);
    return 1;
}
REGISTER_TEST(fb_insert_delete_chars, "Drivers");
