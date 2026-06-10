#pragma once

#include "../boot/limine.h"
#include <stddef.h>
#include <stdint.h>

#define RGB(r, g, b) ((uint32_t) (((r) << 16) | ((g) << 8) | (b)))

#define COLOR_BLACK  RGB(104, 104, 104)   // #686868
#define COLOR_WHITE  RGB(255, 255, 255)   // #FFFFFF
#define COLOR_RED    RGB(253, 113, 120)   // #FD7178
#define COLOR_GREEN  RGB(204, 255, 104)   // #CCFF68
#define COLOR_BLUE   RGB(74, 187, 255)    // #4ABBFF
#define COLOR_YELLOW RGB(255, 167, 60)    // #FFA73C
#define COLOR_CYAN   RGB(89, 255, 198)    // #59FFC6
#define COLOR_GRAY   RGB(198, 198, 198)   // #C6C6C6
#define COLOR_BG     RGB(0, 0, 0)         // #000000

typedef struct
{
    void* addr;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    // for cursor
    uint32_t col, row;
    uint32_t fg, bg;
} fb_t;

extern fb_t g_fb;

void fb_init(struct limine_framebuffer* fb);
void fb_clear(uint32_t color);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

void fb_putchar(char c);
void fb_write(const char* s);
void fb_set_color(uint32_t fg, uint32_t bg);
void fb_cursor_enable(int enable);
void fb_cursor_update(void);
void fb_cursor_blink_tick(uint64_t ticks);
