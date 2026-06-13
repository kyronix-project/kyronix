#include "fb.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include <stdbool.h>

fb_t g_fb;

extern const uint8_t g_psf_font[];

#define FONT_W 8
#define FONT_H 16

/* PSF1: 4-byte header, then 256 glyphs × FONT_H bytes each */
#define PSF1_HDR 4

static bool g_cursor_enabled;
static bool g_cursor_visible = true;
static uint32_t g_cursor_last_col;
static uint32_t g_cursor_last_row;

static void cursor_draw(uint32_t col, uint32_t row, uint32_t color);

void fb_cursor_enable(int enable)
{
    if (!enable && g_cursor_enabled && g_fb.addr)
        cursor_draw(g_cursor_last_col, g_cursor_last_row, g_fb.bg);
    g_cursor_enabled = !!enable;
}

static void cursor_draw(uint32_t col, uint32_t row, uint32_t color)
{
    uint32_t y =
        row * FONT_H +
        (FONT_H -
         1); /* row 15: always blank in this font (other psf fonts can be rendered badly) */
    fb_fill_rect(col * FONT_W, y, FONT_W, 1, color);
}

void fb_cursor_blink_tick(uint64_t ticks)
{
    if (!g_cursor_enabled || !g_fb.addr)
        return;
    static uint64_t last_blink;
    if (ticks - last_blink < 500)
        return;
    last_blink = ticks;
    g_cursor_visible = !g_cursor_visible;
    uint32_t cols = (uint32_t) (g_fb.width / FONT_W);
    uint32_t rows = (uint32_t) (g_fb.height / FONT_H);
    if (g_cursor_last_col < cols && g_cursor_last_row < rows)
        cursor_draw(g_cursor_last_col, g_cursor_last_row, g_cursor_visible ? g_fb.fg : g_fb.bg);
}

void fb_cursor_update(void)
{
    if (!g_cursor_enabled)
        return;

    uint32_t cols = (uint32_t) (g_fb.width / FONT_W);
    uint32_t rows = (uint32_t) (g_fb.height / FONT_H);

    if (g_cursor_last_col < cols && g_cursor_last_row < rows)
        cursor_draw(g_cursor_last_col, g_cursor_last_row, g_fb.bg);

    g_cursor_visible = true;
    if (g_fb.col < cols && g_fb.row < rows)
        cursor_draw(g_fb.col, g_fb.row, g_fb.fg);

    g_cursor_last_col = g_fb.col;
    g_cursor_last_row = g_fb.row;
}

static void draw_char(uint32_t col, uint32_t row, char c)
{
    uint32_t px = col * FONT_W;
    uint32_t py = row * FONT_H;
    const uint8_t* glyph = g_psf_font + PSF1_HDR + (unsigned char) c * FONT_H;

    for (int ri = 0; ri < FONT_H; ri++) {
        uint8_t bits = glyph[ri];
        for (int bit = 7; bit >= 0; bit--) {
            uint32_t color = (bits >> bit) & 1 ? g_fb.fg : g_fb.bg;
            fb_put_pixel(px + (uint32_t) (7 - bit), py + (uint32_t) ri, color);
        }
    }
}

void fb_init(struct limine_framebuffer* fb)
{
    g_fb.addr = fb->address;
    g_fb.phys_addr = virt_to_phys(fb->address);
    g_fb.width = fb->width;
    g_fb.height = fb->height;
    g_fb.pitch = fb->pitch;
    g_fb.bpp = fb->bpp;
    g_fb.col = 0;
    g_fb.row = 0;
    g_fb.fg = COLOR_WHITE;
    g_fb.bg = COLOR_BG;
    g_cursor_enabled = true;
    g_cursor_last_col = 0;
    g_cursor_last_row = 0;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (x >= g_fb.width || y >= g_fb.height)
        return;
    uint32_t* p = (uint32_t*) ((uint8_t*) g_fb.addr + y * g_fb.pitch + x * (g_fb.bpp / 8));
    *p = color;
}

void fb_clear(uint32_t color)
{
    uint8_t* row = g_fb.addr;
    for (uint64_t y = 0; y < g_fb.height; y++) {
        uint32_t* px = (uint32_t*) row;
        for (uint64_t x = 0; x < g_fb.width; x++)
            px[x] = color;
        row += g_fb.pitch;
    }
    g_fb.col = 0;
    g_fb.row = 0;
    g_cursor_last_col = 0;
    g_cursor_last_row = 0;
    if (g_cursor_enabled)
        cursor_draw(g_fb.col, g_fb.row, g_fb.fg);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t dy = 0; dy < h; dy++)
        for (uint32_t dx = 0; dx < w; dx++)
            fb_put_pixel(x + dx, y + dy, color);
}

void fb_set_color(uint32_t fg, uint32_t bg)
{
    g_fb.fg = fg;
    g_fb.bg = bg;
}

static void scroll_up(void)
{
    uint32_t line_bytes = FONT_H * (uint32_t) g_fb.pitch;
    uint8_t* dst = g_fb.addr;
    uint8_t* src = dst + line_bytes;
    uint64_t rows_total = g_fb.height / FONT_H;
    uint64_t copy_bytes = (rows_total - 1) * line_bytes;

    memmove(dst, src, copy_bytes);

    uint32_t last_y = (uint32_t) ((rows_total - 1) * FONT_H);
    fb_fill_rect(0, last_y, (uint32_t) g_fb.width, FONT_H, g_fb.bg);
}

enum { ESC_NONE, ESC_ESC, ESC_CSI } g_esc;
static int g_esc_params[8];
static int g_esc_np;

static const uint32_t ansi_fg[8] = {
    COLOR_BLACK, COLOR_RED,          COLOR_GREEN, COLOR_YELLOW,
    COLOR_BLUE,  RGB(198, 120, 221), COLOR_CYAN,  COLOR_WHITE,
};

static const uint32_t ansi_bg[8] = {
    COLOR_BLACK, COLOR_RED,          COLOR_GREEN, COLOR_YELLOW,
    COLOR_BLUE,  RGB(198, 120, 221), COLOR_CYAN,  COLOR_WHITE,
};

static void fb_erase_to_eol(void)
{
    uint32_t cols = (uint32_t) (g_fb.width / FONT_W);
    uint32_t c = g_fb.col;
    while (c < cols) {
        draw_char(c, g_fb.row, ' ');
        c++;
    }
}

static void fb_sgr(void)
{
    for (int i = 0; i <= g_esc_np; i++) {
        int p = g_esc_params[i];
        switch (p) {
        case 0:
            g_fb.fg = COLOR_WHITE;
            g_fb.bg = COLOR_BG;
            break;
        case 1:
            break;
        case 7: {
            uint32_t t = g_fb.fg;
            g_fb.fg = g_fb.bg;
            g_fb.bg = t;
            break;
        }
        case 30 ... 37:
            g_fb.fg = ansi_fg[p - 30];
            break;
        case 38:
            break;
        case 39:
            g_fb.fg = COLOR_WHITE;
            break;
        case 40 ... 47:
            g_fb.bg = ansi_bg[p - 40];
            break;
        case 48:
            break;
        case 49:
            g_fb.bg = COLOR_BG;
            break;
        case 90:
            g_fb.fg = COLOR_GRAY;
            break;
        case 91 ... 97:
            g_fb.fg = ansi_fg[p - 90];
            break;
        case 100:
            g_fb.bg = COLOR_GRAY;
            break;
        case 101 ... 107:
            g_fb.bg = ansi_bg[p - 100];
            break;
        }
    }
}

void fb_putchar(char c)
{
    uint32_t cols = (uint32_t) (g_fb.width / FONT_W);
    uint32_t rows = (uint32_t) (g_fb.height / FONT_H);

    switch (g_esc) {
    case ESC_ESC:
        g_esc = (c == '[') ? ESC_CSI : ESC_NONE;
        if (g_esc == ESC_CSI) {
            for (int i = 0; i < 8; i++)
                g_esc_params[i] = 0;
            g_esc_np = 0;
        }
        return;
    case ESC_CSI:
        if (c == '\033') {
            g_esc = ESC_ESC;
            return;
        }
        if (c >= '0' && c <= '9') {
            g_esc_params[g_esc_np] = g_esc_params[g_esc_np] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            g_esc_np++;
            if (g_esc_np >= 8)
                g_esc_np = 7;
            g_esc_params[g_esc_np] = 0;
            return;
        }
        g_esc = ESC_NONE;
        if (c == 'm') {
            fb_sgr();
        } else if (c == 'K') {
            fb_erase_to_eol();
        } else if (c == 'D') {
            int n = g_esc_params[0] > 0 ? g_esc_params[0] : 1;
            while (n-- > 0 && g_fb.col > 0)
                g_fb.col--;
        } else if (c == 'G') {
            uint32_t co = (uint32_t) (g_esc_params[0] > 0 ? g_esc_params[0] - 1 : 0);
            if (co < cols)
                g_fb.col = co;
        } else if (c == 'H') {
            uint32_t row = (uint32_t) (g_esc_params[0] > 0 ? g_esc_params[0] - 1 : 0);
            uint32_t col = (uint32_t) (g_esc_params[1] > 0 ? g_esc_params[1] - 1 : 0);
            uint32_t ncols = (uint32_t) (g_fb.width / FONT_W);
            uint32_t nrows = (uint32_t) (g_fb.height / FONT_H);
            if (col < ncols)
                g_fb.col = col;
            if (row < nrows)
                g_fb.row = row;
        } else if (c == 'J') {
            switch (g_esc_params[0]) {
            case 0:
                for (uint32_t c2 = g_fb.col; c2 < g_fb.width / FONT_W; c2++)
                    draw_char(c2, g_fb.row, ' ');
                for (uint32_t r = g_fb.row + 1; r < g_fb.height / FONT_H; r++)
                    for (uint32_t c2 = 0; c2 < g_fb.width / FONT_W; c2++)
                        draw_char(c2, r, ' ');
                break;
            case 1:
                for (uint32_t r = 0; r < g_fb.row; r++)
                    for (uint32_t c2 = 0; c2 < g_fb.width / FONT_W; c2++)
                        draw_char(c2, r, ' ');
                for (uint32_t c2 = 0; c2 <= g_fb.col; c2++)
                    draw_char(c2, g_fb.row, ' ');
                break;
            case 2:
            case 3:
                fb_set_color(COLOR_WHITE, COLOR_BG);
                fb_clear(g_fb.bg);
                break;
            }
        }
        fb_cursor_update();
        return;
    default:
        break;
    }

    if (c == '\033') {
        g_esc = ESC_ESC;
        return;
    }

    if (c == '\n') {
        g_fb.col = 0;
        g_fb.row++;
    } else if (c == '\r') {
        g_fb.col = 0;
    } else if (c == '\t') {
        g_fb.col = (g_fb.col + 8) & ~7u;
    } else if (c == '\b') {
        if (g_fb.col > 0) {
            g_fb.col--;
            draw_char(g_fb.col, g_fb.row, ' ');
        }
    } else {
        draw_char(g_fb.col, g_fb.row, c);
        g_fb.col++;
        if (g_fb.col >= cols) {
            g_fb.col = 0;
            g_fb.row++;
        }
    }

    if (g_fb.row >= rows) {
        scroll_up();
        g_fb.row = rows - 1;
        if (g_cursor_last_row > 0)
            g_cursor_last_row--;
    }

    fb_cursor_update();
}

void fb_write(const char* s)
{
    while (*s)
        fb_putchar(*s++);
}
