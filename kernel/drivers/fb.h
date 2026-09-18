#pragma once

#include "../boot/limine.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RGB(r, g, b) ((uint32_t) (((r) << 16) | ((g) << 8) | (b)))

#define COLOR_BLACK RGB(0, 0, 0)       // #000000
#define COLOR_WHITE RGB(255, 255, 255) // #FFFFFF
#define COLOR_RED RGB(205, 0, 0)       // #CD0000
#define COLOR_GREEN RGB(0, 205, 0)     // #00CD00
#define COLOR_BLUE RGB(0, 0, 205)      // #0000CD
#define COLOR_YELLOW RGB(205, 205, 0)  // #CDCD00
#define COLOR_CYAN RGB(0, 205, 205)    // #00CDCD
#define COLOR_GRAY RGB(160, 160, 160)  // #A0A0A0
#define COLOR_BG RGB(0, 0, 0)          // #000000

#define FONT_W 8
#define FONT_H 16

/* KFNT (Kyronix Font) header — little-endian */
typedef struct {
    char     magic[4];    /* "KFNT" */
    uint16_t version;     /* 1 */
    uint16_t width;       /* pixel width per glyph */
    uint16_t height;      /* pixel height per glyph */
    uint32_t num_glyphs;
    uint32_t flags;       /* reserved */
    uint32_t glyph_off;   /* byte offset to glyph data */
    uint32_t uni_off;     /* byte offset to unicode lookup table */
    uint32_t uni_count;   /* number of entries in unicode table */
} __attribute__((packed)) kfnt_header_t;

/* Unicode lookup entry — sorted by codepoint for binary search */
typedef struct {
    uint32_t codepoint;
    uint16_t glyph_idx;
} __attribute__((packed)) kfnt_uni_entry_t;

typedef struct {
    void *addr;
    uint64_t phys_addr;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    /* cursor */
    uint32_t col, row;
    uint32_t fg, bg;
    bool bold;
    bool cursor_visible;
    /* VT100 charset state */
    bool vt100_graphics;  /* whether VT100 Special Graphics is active */
} fb_t;

extern fb_t g_fb;

void fb_init(struct limine_framebuffer *fb);
void fb_clear(uint32_t color);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

void fb_putchar(char c);
void fb_write(const char *s);
void fb_set_color(uint32_t fg, uint32_t bg);
void fb_cursor_enable(int enable);
void fb_cursor_update(void);
void fb_cursor_blink_tick(uint64_t ticks);
