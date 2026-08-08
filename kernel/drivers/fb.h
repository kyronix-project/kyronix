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

typedef struct {
    char magic[4];
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint32_t num_glyphs;
    uint32_t flags;
    uint32_t glyph_off;
    uint32_t uni_off;
    uint32_t uni_count;
} __attribute__((packed)) kfnt_header_t;

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

    uint32_t col, row;
    uint32_t fg, bg;
    bool bold;
    bool cursor_visible;
    bool vt100_graphics;
} fb_t;

extern fb_t g_fb;

void fb_init(struct limine_framebuffer *fb);
void fb_clear(uint32_t color);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

void fb_putchar(char c);
void fb_write(const char *s);
void fb_begin_write(void);
void fb_end_write(void);
void fb_set_color(uint32_t fg, uint32_t bg);
void fb_cursor_enable(int enable);
void fb_cursor_update(void);
void fb_cursor_blink_tick(uint64_t ticks);
