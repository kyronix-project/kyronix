#include "printf.h"
#include "../drivers/serial.h"
#include "string.h"
#include <stdbool.h>
#include <stdint.h>

static printf_putchar_fn g_putchar = NULL;
static void* g_putchar_ctx = NULL;

void printf_set_putchar(printf_putchar_fn fn, void* ctx)
{
    g_putchar = fn;
    g_putchar_ctx = ctx;
}

typedef struct {
    char* buf;
    size_t size;
    size_t pos;
} buf_ctx_t;

static void buf_putchar(char c, void* ctx)
{
    buf_ctx_t* b = ctx;
    if (b->pos + 1 < b->size)
        b->buf[b->pos++] = c;
}

static void emit(printf_putchar_fn fn, void* ctx, char c, int* count)
{
    fn(c, ctx);
    (*count)++;
}

static void emit_str(printf_putchar_fn fn, void* ctx, const char* s, int width, bool left,
                     int* count)
{
    int len = (int) strlen(s);
    if (!left)
        for (int i = len; i < width; i++)
            emit(fn, ctx, ' ', count);
    while (*s)
        emit(fn, ctx, *s++, count);
    if (left)
        for (int i = len; i < width; i++)
            emit(fn, ctx, ' ', count);
}

static void emit_uint(printf_putchar_fn fn, void* ctx, uint64_t val, int base, bool upper,
                      int width, bool left, bool zero_pad, int* count)
{
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[64];
    int len = 0;
    if (val == 0) {
        buf[len++] = '0';
    } else {
        while (val) {
            buf[len++] = digits[val % base];
            val /= base;
        }
    }
    for (int i = 0, j = len - 1; i < j; i++, j--) {
        char tmp = buf[i];
        buf[i] = buf[j];
        buf[j] = tmp;
    }
    buf[len] = '\0';

    char pad = zero_pad ? '0' : ' ';
    if (!left)
        for (int i = len; i < width; i++)
            emit(fn, ctx, pad, count);
    for (int i = 0; i < len; i++)
        emit(fn, ctx, buf[i], count);
    if (left)
        for (int i = len; i < width; i++)
            emit(fn, ctx, ' ', count);
}

int vprintf_cb(printf_putchar_fn fn, void* ctx, const char* fmt, va_list ap)
{
    int count = 0;

    while (*fmt) {
        if (*fmt != '%') {
            emit(fn, ctx, *fmt++, &count);
            continue;
        }
        fmt++;

        bool left = false;
        bool zero_pad = false;
        int width = 0;

        // flags
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-')
                left = true;
            if (*fmt == '0')
                zero_pad = true;
            fmt++;
        }
        if (left)
            zero_pad = false;

        // width
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        // lmod
        bool is_long = false;
        if (*fmt == 'l') {
            is_long = true;
            fmt++;
        }
        if (*fmt == 'l') {
            fmt++;
        }

        switch (*fmt++) {
        case 'c':
            emit(fn, ctx, (char) va_arg(ap, int), &count);
            break;
        case 's': {
            const char* s = va_arg(ap, const char*);
            emit_str(fn, ctx, s ? s : "(null)", width, left, &count);
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = is_long ? va_arg(ap, int64_t) : va_arg(ap, int);
            if (v < 0) {
                emit(fn, ctx, '-', &count);
                v = -v;
            }
            emit_uint(fn, ctx, (uint64_t) v, 10, false, width, left, zero_pad, &count);
            break;
        }
        case 'u':
            emit_uint(fn, ctx, is_long ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, false,
                      width, left, zero_pad, &count);
            break;
        case 'x':
            emit_uint(fn, ctx, is_long ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, false,
                      width, left, zero_pad, &count);
            break;
        case 'X':
            emit_uint(fn, ctx, is_long ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, true,
                      width, left, zero_pad, &count);
            break;
        case 'o':
            emit_uint(fn, ctx, is_long ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 8, false,
                      width, left, zero_pad, &count);
            break;
        case 'p': {
            uintptr_t p = (uintptr_t) va_arg(ap, void*);
            emit(fn, ctx, '0', &count);
            emit(fn, ctx, 'x', &count);
            emit_uint(fn, ctx, p, 16, false, 16, false, true, &count);
            break;
        }
        case '%':
            emit(fn, ctx, '%', &count);
            break;
        default:
            emit(fn, ctx, '?', &count);
            break;
        }
    }
    return count;
}

int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap)
{
    buf_ctx_t ctx = {buf, size, 0};
    int n = vprintf_cb(buf_putchar, &ctx, fmt, ap);
    if (size)
        buf[ctx.pos] = '\0';
    return n;
}

int snprintf(char* buf, size_t size, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int kprintf(const char* fmt, ...)
{
    if (!g_putchar)
        return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf_cb(g_putchar, g_putchar_ctx, fmt, ap);
    va_end(ap);
    return n;
}

static void serial_putchar_cb(char c, void* ctx)
{
    (void) ctx;
    serial_putchar(COM1, c);
}

int kdbg(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf_cb(serial_putchar_cb, NULL, fmt, ap);
    va_end(ap);
    return n;
}
