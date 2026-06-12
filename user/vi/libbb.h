#ifndef LIBBB_H
#define LIBBB_H

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CONFIG_FEATURE_VI_MAX_LEN 4096
#define CONFIG_FEATURE_VI_UNDO_QUEUE_MAX 256

#define ENABLE_FEATURE_VI_8BIT 0
#define ENABLE_FEATURE_VI_COLON 1
#define ENABLE_FEATURE_VI_CRASHME 0
#define ENABLE_FEATURE_VI_DOT_CMD 1
#define ENABLE_FEATURE_VI_READONLY 1
#define ENABLE_FEATURE_VI_REGEX_SEARCH 0
#define ENABLE_FEATURE_VI_SEARCH 1
#define ENABLE_FEATURE_VI_SET 1
#define ENABLE_FEATURE_VI_SETOPTS 1
#define ENABLE_FEATURE_VI_UNDO 1
#define ENABLE_FEATURE_VI_UNDO_QUEUE 0
#define ENABLE_FEATURE_VI_USE_SIGNALS 1
#define ENABLE_FEATURE_VI_WIN_RESIZE 1
#define ENABLE_FEATURE_VI_YANKMARK 1
#define ENABLE_FEATURE_VI_ASK_TERMINAL 0
#define ENABLE_LOCALE_SUPPORT 0
#define ENABLE_FEATURE_ALLOW_EXEC 1

#define IF_VI(...) __VA_ARGS__

#if ENABLE_FEATURE_VI_COLON
#define IF_FEATURE_VI_COLON(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_COLON(...)
#endif

#if ENABLE_FEATURE_VI_READONLY
#define IF_FEATURE_VI_READONLY(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_READONLY(...)
#endif

#if ENABLE_FEATURE_VI_SEARCH
#define IF_FEATURE_VI_SEARCH(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_SEARCH(...)
#endif

#if ENABLE_FEATURE_VI_YANKMARK
#define IF_FEATURE_VI_YANKMARK(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_YANKMARK(...)
#endif

#if ENABLE_FEATURE_VI_ASK_TERMINAL
#define IF_FEATURE_VI_ASK_TERMINAL(...) __VA_ARGS__
#else
#define IF_FEATURE_VI_ASK_TERMINAL(...)
#endif

#define ARRAY_SIZE(x) ((unsigned) (sizeof(x) / sizeof((x)[0])))

typedef signed char smallint;

#define FALSE 0
#define TRUE 1
#define KEYCODE_BUFFER_SIZE 32

#define KEYCODE_UP -0x100
#define KEYCODE_DOWN -0x101
#define KEYCODE_LEFT -0x102
#define KEYCODE_RIGHT -0x103
#define KEYCODE_HOME -0x104
#define KEYCODE_END -0x105
#define KEYCODE_INSERT -0x106
#define KEYCODE_DELETE -0x107
#define KEYCODE_PAGEUP -0x108
#define KEYCODE_PAGEDOWN -0x109
#define KEYCODE_FUN1 -0x10a
#define KEYCODE_FUN2 -0x10b
#define KEYCODE_FUN3 -0x10c
#define KEYCODE_FUN4 -0x10d
#define KEYCODE_FUN5 -0x10e
#define KEYCODE_FUN6 -0x10f
#define KEYCODE_FUN7 -0x110
#define KEYCODE_FUN8 -0x111
#define KEYCODE_FUN9 -0x112
#define KEYCODE_FUN10 -0x113
#define KEYCODE_FUN11 -0x114
#define KEYCODE_FUN12 -0x115
#define KEYCODE_CURSOR_POS -0x1000

#define UNUSED_PARAM __attribute__((unused))
#define NOINLINE __attribute__((noinline))
#define ALIGN1 __attribute__((aligned(1)))

#define BB_VER "1.36.0"
#define BB_BT "kyronix"

struct globals;
extern struct globals* ptr_to_globals;
#define SET_PTR_TO_GLOBALS(p)                                                                      \
    do                                                                                             \
    {                                                                                              \
        ptr_to_globals = (p);                                                                      \
    } while (0)
#define MAIN_EXTERNALLY_VISIBLE

static inline int bb_putchar(int c)
{
    return putchar(c);
}

static inline void bb_error_msg_and_die(const char* s, ...)
{
    va_list ap;
    va_start(ap, s);
    vfprintf(stderr, s, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static inline void bb_show_usage(void)
{
    fprintf(stderr, "Usage: vi [OPTIONS] [FILE]...\n");
    exit(1);
}

static inline void* xmalloc(size_t size)
{
    void* p = malloc(size);
    if (!p)
    {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return p;
}

static inline void* xzalloc(size_t size)
{
    void* p = calloc(1, size);
    if (!p)
    {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return p;
}

static inline void* xrealloc(void* p, size_t size)
{
    void* q = realloc(p, size);
    if (!q)
    {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return q;
}

static inline char* xstrdup(const char* s)
{
    char* p = strdup(s);
    if (!p)
    {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return p;
}

static inline char* xstrndup(const char* s, size_t n)
{
    char* p = strndup(s, n);
    if (!p)
    {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return p;
}

static inline char* skip_whitespace(const char* s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return (char*) s;
}

static inline char* skip_non_whitespace(const char* s)
{
    while (*s && *s != ' ' && *s != '\t')
        s++;
    return (char*) s;
}

static inline char* last_char_is(const char* s, int c)
{
    if (!s || !*s)
        return NULL;
    size_t sz = strlen(s) - 1;
    return (s[sz] == (char) c) ? (char*) (s + sz) : NULL;
}

int get_terminal_width_height(int, unsigned*, unsigned*);
void tcsetattr_stdin_TCSANOW(const struct termios*);
void fflush_all(void);
ssize_t full_read(int, void*, size_t);
ssize_t full_write(int, const void*, size_t);
int safe_poll(struct pollfd*, nfds_t, int);
int32_t read_key(int, char*, int);

#endif
