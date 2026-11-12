#include "fb.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#ifdef CONFIG_KMEMLEAK
#include "../mm/kmemleak.h"
#endif
#include <stdbool.h>

fb_t g_fb;

extern const uint8_t g_font_data[];

#define FONT_W 8
#define FONT_H 16

static bool g_cursor_enabled;
static bool g_cursor_blink_state = true;
static uint32_t g_cursor_last_col;
static uint32_t g_cursor_last_row;
static uint32_t g_write_depth;

static uint8_t *g_shadow;
static uint64_t g_shadow_bytes;
static uint32_t g_bytes_per_px = 4;

static uint8_t *g_alt;
static bool g_alt_active;

static uint32_t g_scroll_top;
static uint32_t g_scroll_bot;

static uint32_t g_saved_col, g_saved_row;
static uint32_t g_saved_fg, g_saved_bg;
static int g_saved_g0, g_saved_g1, g_saved_gl;
static bool g_saved_bold, g_saved_gfx, g_saved_valid;

static uint32_t g_tab_stops[8];

static void (*g_reply_hook)(const char *buf, uint64_t len);

void fb_set_reply_hook(void (*hook)(const char *buf, uint64_t len)) { g_reply_hook = hook; }

static void fb_reply(const char *s) {
    if (g_reply_hook) g_reply_hook(s, (uint64_t) strlen(s));
}

static inline uint8_t *shadow_active(void) { return (g_alt_active && g_alt) ? g_alt : g_shadow; }

static bool g_console_suspended;

static inline bool console_blackholed(void) { return g_console_suspended && !shadow_active(); }

static inline uint8_t *vram_base(void) {
    return (g_console_suspended && shadow_active()) ? shadow_active() : (uint8_t *) g_fb.addr;
}

static inline uint32_t screen_rows(void) { return (uint32_t) (g_fb.height / FONT_H); }

static inline uint32_t screen_cols(void) { return (uint32_t) (g_fb.width / FONT_W); }

static inline uint8_t *vram_at(uint64_t off) { return vram_base() + off; }

static inline uint64_t px_off(uint32_t x, uint32_t y) {
    return (uint64_t) y * g_fb.pitch + (uint64_t) x * g_bytes_per_px;
}

static void cursor_draw(uint32_t col, uint32_t row, uint32_t color);
static uint32_t adjust_bold(uint32_t color);
static void screen_fill_rows(uint32_t first, uint32_t last, uint32_t color);
static void screen_move_cells(uint32_t row, uint32_t dst_col, uint32_t src_col, uint32_t count);
static void fb_present(void);
static void fb_alloc_alt(void);
static void fb_alt_screen_enter(bool save_cursor);
static void fb_alt_screen_leave(bool restore_cursor);

static inline void screen_write(uint64_t off, uint32_t color) {
    if (!g_console_suspended) *(uint32_t *) ((uint8_t *) g_fb.addr + off) = color;
    uint8_t *s = shadow_active();
    if (s) *(uint32_t *) (s + off) = color;
}

static const kfnt_header_t *g_kfnt;
static const uint8_t *g_glyph_data;
static uint32_t g_bpf;         // bytes per glyph row
static uint32_t g_glyph_bytes; // bytes per glyph (bpf * height)

enum { CS_ASCII = 0, CS_GRAPHICS = 1 };

static int g_charset_g0 = CS_ASCII; // G0 slot assignment
static int g_charset_g1 = CS_ASCII; // G1 slot assignment
static int g_gl = 0;                // 0 = using G0, 1 = using G1
static int g_esc_charset_slot = 0;  // saved slot for ESC_CHARSET

// this table is written by Ai, sorry(

static const uint32_t vt100_upper[] = {
    /* 0x41-0x47 */ 0x2191, 0x2193, 0x2192, 0x2190, 0x2588, 0x259A, 0x2603,
};
static const uint32_t vt100_lower[] = {
    /* 0x60 */ 0x25C6,
    /* 0x61 */ 0x2592,
    /* 0x62-0x69: control chars, identity */
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    /* 0x6A */ 0x2518, /* ┘ */
    /* 0x6B */ 0x2510, /* ┐ */
    /* 0x6C */ 0x250C, /* ┌ */
    /* 0x6D */ 0x2514, /* └ */
    /* 0x6E */ 0x253C, /* ┼ */
    /* 0x6F */ 0x23BA, /* ⎺  (scan 1) */
    /* 0x70 */ 0x23BB, /* ⎻  (scan 3) */
    /* 0x71 */ 0x2500, /* ─  (horiz line) */
    /* 0x72 */ 0x23BC, /* ⎼  (scan 7) */
    /* 0x73 */ 0x23BD, /* ⎽  (scan 9) */
    /* 0x74 */ 0x251C, /* ├ */
    /* 0x75 */ 0x2524, /* ┤ */
    /* 0x76 */ 0x2534, /* ┴ */
    /* 0x77 */ 0x252C, /* ┬ */
    /* 0x78 */ 0x2502, /* │ */
    /* 0x79 */ 0x2264, /* ≤ */
    /* 0x7A */ 0x2265, /* ≥ */
    /* 0x7B */ 0x3C0,  /* π */
    /* 0x7C */ 0x2260, /* ≠ */
    /* 0x7D */ 0x00A3, /* £ */
    /* 0x7E */ 0x00B7, /* · */
};

static int font_find_glyph(uint32_t cp) {
    const kfnt_uni_entry_t *table =
        (const kfnt_uni_entry_t *) ((const uint8_t *) g_kfnt + g_kfnt->uni_off);
    uint32_t lo = 0, hi = g_kfnt->uni_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (table[mid].codepoint < cp)
            lo = mid + 1;
        else if (table[mid].codepoint > cp)
            hi = mid;
        else
            return table[mid].glyph_idx;
    }
    return -1;
}

static uint32_t vt100_resolve(char c) {
    unsigned char uc = (unsigned char) c;
    if (uc >= 0x41 && uc <= 0x47) return vt100_upper[uc - 0x41];
    if (uc >= 0x60 && uc <= 0x7E) return vt100_lower[uc - 0x60];
    return 0;
}

void fb_cursor_enable(int enable) {
    if (!enable && g_cursor_enabled && g_fb.addr)
        cursor_draw(g_cursor_last_col, g_cursor_last_row, g_fb.bg);
    g_cursor_enabled = !!enable;
}

static void cursor_draw(uint32_t col, uint32_t row, uint32_t color) {
    uint32_t y = row * FONT_H + (FONT_H - 1);
    fb_fill_rect(col * FONT_W, y, FONT_W, 1, color);
}

void fb_cursor_blink_tick(uint64_t ticks) {
    if (!g_cursor_enabled || !g_fb.addr) return;
    if (!g_fb.cursor_visible) return;
    static uint64_t last_blink;
    if (ticks - last_blink < 500) return;
    last_blink = ticks;
    g_cursor_blink_state = !g_cursor_blink_state;
    uint32_t cols = (uint32_t) (g_fb.width / FONT_W);
    uint32_t rows = (uint32_t) (g_fb.height / FONT_H);
    if (g_cursor_last_col < cols && g_cursor_last_row < rows)
        cursor_draw(g_cursor_last_col, g_cursor_last_row, g_cursor_blink_state ? g_fb.fg : g_fb.bg);
}

void fb_cursor_update(void) {
    if (g_write_depth) return;
    if (!g_cursor_enabled) return;
    uint32_t cols = (uint32_t) (g_fb.width / FONT_W);
    uint32_t rows = (uint32_t) (g_fb.height / FONT_H);
    if (g_cursor_last_col < cols && g_cursor_last_row < rows)
        cursor_draw(g_cursor_last_col, g_cursor_last_row, g_fb.bg);
    g_cursor_blink_state = true;
    if (g_fb.col < cols && g_fb.row < rows) cursor_draw(g_fb.col, g_fb.row, g_fb.fg);
    g_cursor_last_col = g_fb.col;
    g_cursor_last_row = g_fb.row;
}

void fb_begin_write(void) { g_write_depth++; }

void fb_end_write(void) {
    if (g_write_depth && --g_write_depth == 0) fb_cursor_update();
}

static void draw_char(uint32_t col, uint32_t row, uint32_t glyph_idx) {
    uint32_t px = col * FONT_W;
    uint32_t py = row * FONT_H;
    if (px + FONT_W > g_fb.width || py + FONT_H > g_fb.height) return;

    const uint8_t *glyph = g_glyph_data + glyph_idx * g_glyph_bytes;
    uint32_t fg = g_fb.bold ? adjust_bold(g_fb.fg) : g_fb.fg;
    uint32_t bg = g_fb.bg;

    for (uint32_t ri = 0; ri < FONT_H; ri++) {
        uint8_t bits = glyph[ri * g_bpf];
        uint64_t off = px_off(px, py + ri);
        for (uint32_t b = 0; b < FONT_W; b++)
            screen_write(off + b * g_bytes_per_px, (bits & (0x80u >> b)) ? fg : bg);
    }
}

static void fb_alloc_alt(void) {
    if (g_alt || !g_shadow_bytes) return;
    uint64_t pages = (g_shadow_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    void *phys = pmm_alloc_contiguous(pages);
    if (!phys) return;
    g_alt = (uint8_t *) phys_to_virt((uint64_t) phys);
    memset(g_alt, 0, g_shadow_bytes);
#ifdef CONFIG_KMEMLEAK
    for (uint64_t i = 0; i < pages; i++)
        kmemleak_untrack_page((void *) ((uint64_t) phys + i * PAGE_SIZE));
#endif
}

static void fb_present(void) {
    if (g_console_suspended || !g_fb.addr) return;
    uint8_t *s = shadow_active();
    if (s) memcpy(g_fb.addr, s, g_shadow_bytes);
}

void fb_init(struct limine_framebuffer *fb) {
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
    g_fb.bold = false;
    g_fb.cursor_visible = true;
    g_fb.vt100_graphics = false;
    g_cursor_enabled = true;
    g_cursor_last_col = 0;
    g_cursor_last_row = 0;

    // parse kfnt font header
    g_kfnt = (const kfnt_header_t *) g_font_data;
    g_glyph_data = g_font_data + g_kfnt->glyph_off;
    g_bpf = (g_kfnt->width + 7) / 8;
    g_glyph_bytes = g_bpf * g_kfnt->height;

    g_bytes_per_px = g_fb.bpp / 8 ? g_fb.bpp / 8 : 4;
    g_shadow_bytes = (uint64_t) g_fb.pitch * g_fb.height;
    uint64_t pages = (g_shadow_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    void *phys = pmm_alloc_contiguous(pages);
    g_shadow = phys ? (uint8_t *) phys_to_virt((uint64_t) phys) : NULL;
    if (g_shadow) {
        memset(g_shadow, 0, g_shadow_bytes);
#ifdef CONFIG_KMEMLEAK
        for (uint64_t i = 0; i < pages; i++)
            kmemleak_untrack_page((void *) ((uint64_t) phys + i * PAGE_SIZE));
#endif
    }

    g_scroll_top = 0;
    g_scroll_bot = screen_rows() ? screen_rows() - 1 : 0;
    for (int i = 0; i < 8; i++) g_tab_stops[i] = (uint32_t) ((i + 1) * 8);
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= g_fb.width || y >= g_fb.height) return;
    if (console_blackholed()) return;
    screen_write(px_off(x, y), color);
}

void fb_clear(uint32_t color) {
    if (console_blackholed()) return;
    uint8_t *s = shadow_active();
    for (uint64_t y = 0; y < g_fb.height; y++) {
        uint64_t off = px_off(0, (uint32_t) y);
        uint32_t *v = (uint32_t *) ((uint8_t *) g_fb.addr + off);
        for (uint64_t x = 0; x < g_fb.width; x++) {
            if (!g_console_suspended) v[x] = color;
            if (s) ((uint32_t *) (s + off))[x] = color;
        }
    }
    g_fb.col = 0;
    g_fb.row = 0;
    g_cursor_last_col = 0;
    g_cursor_last_row = 0;
    if (g_cursor_enabled) cursor_draw(g_fb.col, g_fb.row, g_fb.fg);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (x >= g_fb.width || y >= g_fb.height) return;
    if (console_blackholed()) return;
    if (w > g_fb.width - x) w = (uint32_t) (g_fb.width - x);
    if (h > g_fb.height - y) h = (uint32_t) (g_fb.height - y);
    for (uint32_t dy = 0; dy < h; dy++) {
        uint64_t off = px_off(x, y + dy);
        for (uint32_t dx = 0; dx < w; dx++) screen_write(off + dx * g_bytes_per_px, color);
    }
}

void fb_set_color(uint32_t fg, uint32_t bg) {
    g_fb.fg = fg;
    g_fb.bg = bg;
}

static void screen_move_rows(uint32_t dst_row, uint32_t src_row, uint32_t count) {
    if (!count) return;
    uint64_t line_bytes = (uint64_t) FONT_H * g_fb.pitch;
    uint8_t *s = shadow_active();
    uint64_t dst_off = (uint64_t) dst_row * line_bytes;
    uint64_t src_off = (uint64_t) src_row * line_bytes;
    uint64_t n = (uint64_t) count * line_bytes;

    if (!g_console_suspended)
        memmove((uint8_t *) g_fb.addr + dst_off, (uint8_t *) g_fb.addr + src_off, n);
    if (s) memmove(s + dst_off, s + src_off, n);
}

static void screen_fill_rows(uint32_t first, uint32_t last, uint32_t color) {
    for (uint32_t r = first; r <= last; r++)
        fb_fill_rect(0, r * FONT_H, (uint32_t) g_fb.width, FONT_H, color);
}

static void screen_move_cells(uint32_t row, uint32_t dst_col, uint32_t src_col, uint32_t count) {
    if (!count) return;
    uint64_t row_off = (uint64_t) row * FONT_H * g_fb.pitch;
    uint64_t dst_off = row_off + (uint64_t) dst_col * FONT_W * g_bytes_per_px;
    uint64_t src_off = row_off + (uint64_t) src_col * FONT_W * g_bytes_per_px;
    uint64_t n = (uint64_t) count * FONT_W * g_bytes_per_px;
    uint8_t *s = shadow_active();

    for (uint32_t dy = 0; dy < FONT_H; dy++) {
        uint64_t off = (uint64_t) dy * g_fb.pitch;
        if (!g_console_suspended)
            memmove((uint8_t *) g_fb.addr + dst_off + off,
                    (uint8_t *) g_fb.addr + src_off + off, n);
        if (s) memmove(s + dst_off + off, s + src_off + off, n);
    }
}

static void fb_alt_screen_enter(bool save_cursor) {
    uint32_t cols = screen_cols();
    uint32_t rows = screen_rows();
    if (save_cursor) {
        g_saved_col = g_fb.col;
        g_saved_row = g_fb.row;
        g_saved_valid = true;
    }
    if (!g_alt) fb_alloc_alt();
    if (!g_alt) return;

    g_alt_active = true;
    if (rows) screen_fill_rows(0, rows - 1, g_fb.bg);
    if (save_cursor) {
        g_fb.col = 0;
        g_fb.row = 0;
    }
    if (cols && g_fb.col >= cols) g_fb.col = cols - 1;
    if (rows && g_fb.row >= rows) g_fb.row = rows - 1;
    fb_present();
}

static void fb_alt_screen_leave(bool restore_cursor) {
    if (!g_alt_active) return;
    g_alt_active = false;
    if (restore_cursor && g_saved_valid) {
        g_fb.col = g_saved_col;
        g_fb.row = g_saved_row;
    }
    fb_present();
}

static void scroll_up(void) {
    if (console_blackholed()) return;
    uint32_t rows_total = screen_rows();
    if (rows_total < 2) return;

    uint32_t top = g_scroll_top < rows_total ? g_scroll_top : 0;
    uint32_t bot = g_scroll_bot < rows_total ? g_scroll_bot : rows_total - 1;
    if (top >= bot) {
        top = 0;
        bot = rows_total - 1;
    }

    screen_move_rows(top, top + 1, bot - top);
    screen_fill_rows(bot, bot, g_fb.bg);
}

static void scroll_down(void) {
    if (console_blackholed()) return;
    uint32_t rows_total = screen_rows();
    if (rows_total < 2) return;

    uint32_t top = g_scroll_top < rows_total ? g_scroll_top : 0;
    uint32_t bot = g_scroll_bot < rows_total ? g_scroll_bot : rows_total - 1;
    if (top >= bot) {
        top = 0;
        bot = rows_total - 1;
    }

    screen_move_rows(top + 1, top, bot - top);
    screen_fill_rows(top, top, g_fb.bg);
}

static uint32_t region_bottom(void) {
    uint32_t rows_total = screen_rows();
    return g_scroll_bot < rows_total ? g_scroll_bot : (rows_total ? rows_total - 1 : 0);
}

static uint32_t region_top(void) {
    uint32_t rows_total = screen_rows();
    return g_scroll_top < rows_total ? g_scroll_top : 0;
}

static void index_down(void) {
    uint32_t rows_total = screen_rows();
    if (!rows_total) return;
    if (g_fb.row == region_bottom()) {
        scroll_up();
    } else if (g_fb.row < rows_total - 1) {
        g_fb.row++;
    }
}

static void reverse_index(void) {
    if (g_fb.row == region_top()) {
        scroll_down();
    } else if (g_fb.row > 0) {
        g_fb.row--;
    }
}

void fb_console_suspend(int suspend) {
    bool off = suspend != 0;
    if (off == g_console_suspended) return;
    g_console_suspended = off;
    if (off) return;
    fb_present();
    if (!shadow_active()) fb_clear(g_fb.bg);
    fb_cursor_update();
}

enum { ESC_NONE, ESC_ESC, ESC_CSI, ESC_CHARSET, ESC_OSC, ESC_DCS } g_esc;
static int g_esc_params[16];
static int g_esc_np;
static bool g_esc_priv;
static char g_esc_inter;
static char g_dcs[16];
static int g_dcs_len;

static const uint32_t ansi_fg[8] = {
    COLOR_BLACK, COLOR_RED,          COLOR_GREEN, COLOR_YELLOW,
    COLOR_BLUE,  RGB(198, 120, 221), COLOR_CYAN,  COLOR_WHITE,
};

static const uint32_t ansi_bg[8] = {
    COLOR_BLACK, COLOR_RED,          COLOR_GREEN, COLOR_YELLOW,
    COLOR_BLUE,  RGB(198, 120, 221), COLOR_CYAN,  COLOR_WHITE,
};

static const uint32_t ansi_bright[8] = {
    RGB(128, 128, 128), RGB(255, 85, 85),   RGB(150, 255, 85), RGB(255, 255, 85),
    RGB(85, 170, 255),  RGB(215, 150, 255), RGB(85, 255, 255), RGB(255, 255, 255),
};

static uint32_t adjust_bold(uint32_t color) {
    for (int i = 0; i < 8; i++) {
        if (color == ansi_fg[i]) return ansi_bright[i];
    }
    return color;
}

static uint32_t ansi_256(int idx) {
    if (idx < 0) return COLOR_BLACK;
    if (idx < 8) return ansi_fg[idx];
    if (idx < 16) return ansi_bright[idx - 8];
    if (idx < 232) {
        idx -= 16;
        int r = (idx / 36) * 255 / 5;
        int g = ((idx / 6) % 6) * 255 / 5;
        int b = (idx % 6) * 255 / 5;
        if (r == 0) r = 0;
        if (g == 0) g = 0;
        if (b == 0) b = 0;
        return RGB(r, g, b);
    }
    if (idx < 256) {
        int v = (idx - 232) * 256 / 24 + 8;
        if (v > 255) v = 255;
        return RGB(v, v, v);
    }
    return COLOR_WHITE;
}

static void fb_erase_to_eol(void) {
    uint32_t cols = (uint32_t) (g_fb.width / FONT_W);
    uint32_t c = g_fb.col;
    while (c < cols) {
        draw_char(c, g_fb.row, ' ');
        c++;
    }
}

static void fb_sgr(void) {
    for (int i = 0; i <= g_esc_np; i++) {
        int p = g_esc_params[i];
        switch (p) {
        case 0:
            g_fb.fg = COLOR_WHITE;
            g_fb.bg = COLOR_BG;
            g_fb.bold = false;
            break;
        case 1:
            g_fb.bold = true;
            break;
        case 2:
        case 22:
            g_fb.bold = false;
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
            if (i + 1 <= g_esc_np) {
                i++;
                int st = g_esc_params[i];
                if (st == 5 && i + 1 <= g_esc_np) {
                    i++;
                    g_fb.fg = ansi_256(g_esc_params[i]);
                } else if (st == 2 && i + 3 <= g_esc_np) {
                    i++;
                    int r = g_esc_params[i];
                    i++;
                    int gv = g_esc_params[i];
                    i++;
                    int b = g_esc_params[i];
                    g_fb.fg = RGB(r & 0xFF, gv & 0xFF, b & 0xFF);
                }
            }
            break;
        case 39:
            g_fb.fg = COLOR_WHITE;
            break;
        case 40 ... 47:
            g_fb.bg = ansi_bg[p - 40];
            break;
        case 48:
            if (i + 1 <= g_esc_np) {
                i++;
                int st = g_esc_params[i];
                if (st == 5 && i + 1 <= g_esc_np) {
                    i++;
                    g_fb.bg = ansi_256(g_esc_params[i]);
                } else if (st == 2 && i + 3 <= g_esc_np) {
                    i++;
                    int r = g_esc_params[i];
                    i++;
                    int gv = g_esc_params[i];
                    i++;
                    int b = g_esc_params[i];
                    g_fb.bg = RGB(r & 0xFF, gv & 0xFF, b & 0xFF);
                }
            }
            break;
        case 49:
            g_fb.bg = COLOR_BG;
            break;
        case 90:
            g_fb.fg = ansi_bright[0];
            break;
        case 91 ... 97:
            g_fb.fg = ansi_bright[p - 91];
            break;
        case 100:
            g_fb.bg = ansi_bright[0];
            break;
        case 101 ... 107:
            g_fb.bg = ansi_bright[p - 101];
            break;
        }
    }
}

void fb_putchar(char c) {
    uint32_t cols = (uint32_t) (g_fb.width / FONT_W);
    uint32_t rows = (uint32_t) (g_fb.height / FONT_H);

    switch (g_esc) {
    case ESC_OSC:
        if (c == '\a') g_esc = ESC_NONE;
        if (c == '\033') g_esc = ESC_ESC;
        return;

    case ESC_DCS:
        if (c == '\033') {
            if (g_dcs_len == 3 && g_dcs[0] == '$' && g_dcs[1] == 'q' && g_dcs[2] == 'm')
                fb_reply("\033P1$r0m\033\\");
            g_dcs_len = 0;
            g_esc = ESC_ESC;
            return;
        }
        if (g_dcs_len < (int) sizeof(g_dcs)) g_dcs[g_dcs_len++] = (char) c;
        return;

    case ESC_ESC:
        if (c == '[') {
            g_esc = ESC_CSI;
            for (int i = 0; i < 16; i++) g_esc_params[i] = 0;
            g_esc_np = 0;
            g_esc_priv = false;
            g_esc_inter = 0;
        } else if (c == ']') {
            g_esc = ESC_OSC;
        } else if (c == 'P') {
            g_esc = ESC_DCS;
            g_dcs_len = 0;
        } else if (c == '(' || c == ')') {
            g_esc_charset_slot = (c == ')') ? 1 : 0;
            g_esc = ESC_CHARSET;
        } else {
            g_esc = ESC_NONE;
            if (c == '7') {
                g_saved_col = g_fb.col;
                g_saved_row = g_fb.row;
                g_saved_fg = g_fb.fg;
                g_saved_bg = g_fb.bg;
                g_saved_bold = g_fb.bold;
                g_saved_g0 = g_charset_g0;
                g_saved_g1 = g_charset_g1;
                g_saved_gl = g_gl;
                g_saved_gfx = g_fb.vt100_graphics;
                g_saved_valid = true;
            } else if (c == '8') {
                if (g_saved_valid) {
                    g_fb.col = g_saved_col;
                    g_fb.row = g_saved_row;
                    g_fb.fg = g_saved_fg;
                    g_fb.bg = g_saved_bg;
                    g_fb.bold = g_saved_bold;
                    g_charset_g0 = g_saved_g0;
                    g_charset_g1 = g_saved_g1;
                    g_gl = g_saved_gl;
                    g_fb.vt100_graphics = g_saved_gfx;
                }
            } else if (c == 'c') {
                // RIS: full reset
                g_fb.fg = COLOR_WHITE;
                g_fb.bg = COLOR_BG;
                g_fb.bold = false;
                g_fb.vt100_graphics = false;
                g_charset_g0 = CS_ASCII;
                g_charset_g1 = CS_ASCII;
                g_gl = 0;
                fb_clear(COLOR_BG);
            } else if (c == 'D') {
                // IND: index - move cursor down, scroll if at the region bottom
                index_down();
            } else if (c == 'E') {
                // NEL: next line
                g_fb.col = 0;
                index_down();
            } else if (c == 'M') {
                // RI: reverse index
                reverse_index();
            }
        }
        return;

    case ESC_CHARSET: {
        g_esc = ESC_NONE;
        int slot = g_esc_charset_slot;
        if (c == '0') {
            if (slot == 0)
                g_charset_g0 = CS_GRAPHICS;
            else
                g_charset_g1 = CS_GRAPHICS;
        } else if (c == 'B') {
            if (slot == 0)
                g_charset_g0 = CS_ASCII;
            else
                g_charset_g1 = CS_ASCII;
        }
        // upd active graphics state
        int active = (g_gl == 0) ? g_charset_g0 : g_charset_g1;
        g_fb.vt100_graphics = (active == CS_GRAPHICS);
        return;
    }

    // csi
    case ESC_CSI:
        if (c == '?') {
            g_esc_priv = true;
            return;
        }
        if (c == '\033') {
            g_esc = ESC_ESC;
            return;
        }
        if (c >= '0' && c <= '9') {
            g_esc_params[g_esc_np] = g_esc_params[g_esc_np] * 10 + (c - '0');
            return;
        }
        if (c == ';' || c == ':') {
            g_esc_np++;
            if (g_esc_np >= 16) g_esc_np = 15;
            g_esc_params[g_esc_np] = 0;
            return;
        }
        if (c >= 0x20 && c <= 0x2F) {
            g_esc_inter = c;
            return;
        }
        if (c >= 0x3C && c <= 0x3F) {
            g_esc_inter = c;
            return;
        }
        g_esc = ESC_NONE;

        if (g_esc_priv && g_esc_inter == '$' && c == 'p') {
            char out[24];
            int n = 0;
            int v = g_esc_params[0];
            char num[8];
            int dn = 0;
            out[n++] = '\033';
            out[n++] = '[';
            out[n++] = '?';
            if (v == 0) num[dn++] = '0';
            while (v > 0 && dn < 7) {
                num[dn++] = (char) ('0' + v % 10);
                v /= 10;
            }
            while (dn > 0) out[n++] = num[--dn];
            out[n++] = ';';
            out[n++] = '0';
            out[n++] = '$';
            out[n++] = 'y';
            out[n] = '\0';
            g_esc_priv = false;
            fb_reply(out);
            return;
        }
        if (g_esc_priv && c == 'u') {
            g_esc_priv = false;
            fb_reply("\033[?0u");
            return;
        }

        // Private/DEC mode: ESC[?...h / ESC[?...l
        if (g_esc_priv) {
            int p = g_esc_params[0];
            if (c == 'h') {
                if (p == 25) {
                    g_fb.cursor_visible = true;
                    g_cursor_enabled = true;
                } else if (p == 7) {
                    // DECAWM: auto wrap - always on
                } else if (p == 1) {
                    // DECCKM: cursor key mode
                } else if (p == 47) {
                    fb_alt_screen_enter(false);
                } else if (p == 1049) {
                    fb_alt_screen_enter(true);
                } else if (p == 2004) {
                    // bracketed paste - not supported
                }
            } else if (c == 'l') {
                if (p == 25) {
                    g_fb.cursor_visible = false;
                    g_cursor_enabled = false;
                    cursor_draw(g_fb.col, g_fb.row, g_fb.bg);
                } else if (p == 7) {
                    // DECAWM off
                } else if (p == 1) {
                    // DECCKM off
                } else if (p == 47) {
                    fb_alt_screen_leave(false);
                } else if (p == 1049) {
                    fb_alt_screen_leave(true);
                } else if (p == 2004) {
                    // disable bracketed paste
                }
            }
            g_esc_priv = false;
            fb_cursor_update();
            return;
        }

        if (c == 'm') {
            fb_sgr();
        } else if (c == 'K') {
            int n = g_esc_params[0];
            if (n == 0) {
                fb_erase_to_eol();
            } else if (n == 1) {
                for (uint32_t c2 = 0; c2 <= g_fb.col && c2 < cols; c2++)
                    draw_char(c2, g_fb.row, ' ');
            } else if (n == 2) {
                for (uint32_t c2 = 0; c2 < cols; c2++) draw_char(c2, g_fb.row, ' ');
            }
        } else if (c == 'A') {
            int n = g_esc_params[0] > 0 ? g_esc_params[0] : 1;
            while (n-- > 0 && g_fb.row > 0) g_fb.row--;
        } else if (c == 'B') {
            int n = g_esc_params[0] > 0 ? g_esc_params[0] : 1;
            uint32_t maxrow = rows > 0 ? rows - 1 : 0;
            while (n-- > 0 && g_fb.row < maxrow) g_fb.row++;
        } else if (c == 'C') {
            int n = g_esc_params[0] > 0 ? g_esc_params[0] : 1;
            uint32_t maxcol = cols > 0 ? cols - 1 : 0;
            while (n-- > 0 && g_fb.col < maxcol) g_fb.col++;
        } else if (c == 'D') {
            int n = g_esc_params[0] > 0 ? g_esc_params[0] : 1;
            while (n-- > 0 && g_fb.col > 0) g_fb.col--;
        } else if (c == 'G') {
            uint32_t co = (uint32_t) (g_esc_params[0] > 0 ? g_esc_params[0] - 1 : 0);
            if (co < cols) g_fb.col = co;
        } else if (c == 'H' || c == 'f') {
            uint32_t row = (uint32_t) (g_esc_params[0] > 0 ? g_esc_params[0] - 1 : 0);
            uint32_t col = (uint32_t) (g_esc_params[1] > 0 ? g_esc_params[1] - 1 : 0);
            if (col < cols) g_fb.col = col;
            if (row < rows) g_fb.row = row;
        } else if (c == 'J') {
            switch (g_esc_params[0]) {
            case 0:
                for (uint32_t c2 = g_fb.col; c2 < cols; c2++) draw_char(c2, g_fb.row, ' ');
                for (uint32_t r = g_fb.row + 1; r < rows; r++)
                    for (uint32_t c2 = 0; c2 < cols; c2++) draw_char(c2, r, ' ');
                break;
            case 1:
                for (uint32_t r = 0; r < g_fb.row; r++)
                    for (uint32_t c2 = 0; c2 < cols; c2++) draw_char(c2, r, ' ');
                for (uint32_t c2 = 0; c2 <= g_fb.col; c2++) draw_char(c2, g_fb.row, ' ');
                break;
            case 2:
            case 3:
                fb_clear(g_fb.bg);
                break;
            }
        } else if (c == 'X') {
            int n = g_esc_params[0] > 0 ? g_esc_params[0] : 1;
            for (int i = 0; i < n && g_fb.col + i < cols; i++)
                draw_char(g_fb.col + i, g_fb.row, ' ');
        } else if (c == 's') {
            g_saved_col = g_fb.col;
            g_saved_row = g_fb.row;
            g_saved_valid = true;
        } else if (c == 'u') {
            if (g_saved_valid) {
                g_fb.col = g_saved_col;
                g_fb.row = g_saved_row;
            }
        } else if (c == 'r') {
            uint32_t top = (uint32_t) (g_esc_params[0] > 0 ? g_esc_params[0] - 1 : 0);
            uint32_t bot = g_esc_params[1] > 0 ? (uint32_t) (g_esc_params[1] - 1) :
                                                 (rows ? rows - 1 : 0);
            if (bot >= rows) bot = rows ? rows - 1 : 0;
            if (top < bot) {
                g_scroll_top = top;
                g_scroll_bot = bot;
            } else {
                g_scroll_top = 0;
                g_scroll_bot = rows ? rows - 1 : 0;
            }
            g_fb.col = 0;
            g_fb.row = g_scroll_top;
        } else if (c == 'L') {
            int n = g_esc_params[0] > 0 ? g_esc_params[0] : 1;
            uint32_t bot = g_scroll_bot < rows ? g_scroll_bot : (rows ? rows - 1 : 0);
            if (g_fb.row >= g_scroll_top && g_fb.row <= bot) {
                if ((uint32_t) n > bot - g_fb.row + 1) n = (int) (bot - g_fb.row + 1);
                uint32_t room = bot - g_fb.row + 1 - (uint32_t) n;
                if (room) screen_move_rows(g_fb.row + (uint32_t) n, g_fb.row, room);
                screen_fill_rows(g_fb.row, g_fb.row + (uint32_t) n - 1, g_fb.bg);
                g_fb.col = 0;
            }
        } else if (c == 'M') {
            int n = g_esc_params[0] > 0 ? g_esc_params[0] : 1;
            uint32_t bot = g_scroll_bot < rows ? g_scroll_bot : (rows ? rows - 1 : 0);
            if (g_fb.row >= g_scroll_top && g_fb.row <= bot) {
                if ((uint32_t) n > bot - g_fb.row + 1) n = (int) (bot - g_fb.row + 1);
                uint32_t room = bot - g_fb.row + 1 - (uint32_t) n;
                if (room) screen_move_rows(g_fb.row, g_fb.row + (uint32_t) n, room);
                screen_fill_rows(bot - (uint32_t) n + 1, bot, g_fb.bg);
                g_fb.col = 0;
            }
        } else if (c == 'P') {
            int n = g_esc_params[0] > 0 ? g_esc_params[0] : 1;
            if (g_fb.col < cols) {
                if ((uint32_t) n > cols - g_fb.col) n = (int) (cols - g_fb.col);
                screen_move_cells(g_fb.row, g_fb.col, g_fb.col + (uint32_t) n,
                                  cols - g_fb.col - (uint32_t) n);
                for (uint32_t x = cols - (uint32_t) n; x < cols; x++)
                    draw_char(x, g_fb.row, ' ');
            }
        } else if (c == '@') {
            int n = g_esc_params[0] > 0 ? g_esc_params[0] : 1;
            if (g_fb.col < cols) {
                if ((uint32_t) n > cols - g_fb.col) n = (int) (cols - g_fb.col);
                screen_move_cells(g_fb.row, g_fb.col + (uint32_t) n, g_fb.col,
                                  cols - g_fb.col - (uint32_t) n);
                for (uint32_t k = 0; k < (uint32_t) n; k++)
                    draw_char(g_fb.col + k, g_fb.row, ' ');
            }
        } else if (c == 'S') {
            int n = g_esc_params[0] > 0 ? g_esc_params[0] : 1;
            while (n-- > 0) scroll_up();
        } else if (c == 'T') {
            int n = g_esc_params[0] > 0 ? g_esc_params[0] : 1;
            while (n-- > 0) scroll_down();
        } else if (c == 'd') {
            uint32_t row = (uint32_t) (g_esc_params[0] > 0 ? g_esc_params[0] - 1 : 0);
            if (row < rows) g_fb.row = row;
        } else if (c == 'g') {
            if (g_esc_params[0] == 3) {
                for (int i = 0; i < 8; i++) g_tab_stops[i] = 0;
            } else {
                uint32_t cell = g_fb.col / 8;
                if (cell < 8) g_tab_stops[cell] = 0;
            }
        } else if (c == 'n') {
            if (g_esc_params[0] == 6) {
                char out[32];
                int len = 0;
                uint32_t v = g_fb.row + 1;
                out[len++] = '\033';
                out[len++] = '[';
                if (v >= 10) out[len++] = (char) ('0' + v / 10);
                out[len++] = (char) ('0' + v % 10);
                out[len++] = ';';
                v = g_fb.col + 1;
                if (v >= 10) out[len++] = (char) ('0' + v / 10);
                out[len++] = (char) ('0' + v % 10);
                out[len++] = 'R';
                out[len] = '\0';
                fb_reply(out);
            }
        } else if (c == 'c') {
            fb_reply("\033[?1;2c");
        } else if (c == '~') {
            // ESC[N~ sequences. pass
        }
        fb_cursor_update();
        return;

    default:
        break;
    }

    // SO - switch to G1 charset
    if (c == 0x0E) {
        g_gl = 1;
        int active = (g_gl == 0) ? g_charset_g0 : g_charset_g1;
        g_fb.vt100_graphics = (active == CS_GRAPHICS);
        return;
    }
    // SI - switch to G0 charset
    if (c == 0x0F) {
        g_gl = 0;
        int active = (g_gl == 0) ? g_charset_g0 : g_charset_g1;
        g_fb.vt100_graphics = (active == CS_GRAPHICS);
        return;
    }

    if (c == '\033') {
        g_esc = ESC_ESC;
        return;
    }

    if (c == '\n') {
        g_fb.col = 0;
        index_down();
    } else if (c == '\r') {
        g_fb.col = 0;
    } else if (c == '\t') {
        uint32_t next = cols;
        for (int i = 0; i < 8; i++) {
            if (g_tab_stops[i] && g_tab_stops[i] > g_fb.col) {
                next = g_tab_stops[i];
                break;
            }
        }
        g_fb.col = next < cols ? next : (cols ? cols - 1 : 0);
    } else if (c == '\b') {
        if (g_fb.col > 0) {
            g_fb.col--;
            draw_char(g_fb.col, g_fb.row, ' ');
        }
    } else {
        uint32_t glyph_idx = (unsigned char) c;

        // vt100 sgt
        if (g_fb.vt100_graphics) {
            uint32_t ucp = vt100_resolve(c);
            if (ucp) {
                int gi = font_find_glyph(ucp);
                if (gi >= 0) glyph_idx = (uint32_t) gi;
            }
        }

        draw_char(g_fb.col, g_fb.row, glyph_idx);
        g_fb.col++;
        if (g_fb.col >= cols) {
            g_fb.col = 0;
            if (g_fb.row == region_bottom()) {
                scroll_up();
            } else {
                g_fb.row++;
            }
        }
    }

    if (g_fb.row >= rows) {
        scroll_up();
        g_fb.row = rows - 1;
        if (g_cursor_last_row > 0) g_cursor_last_row--;
    }

    fb_cursor_update();
}

void fb_write(const char *s) {
    fb_begin_write();
    while (*s) fb_putchar(*s++);
    fb_end_write();
}
