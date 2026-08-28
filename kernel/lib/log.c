#include "lib/log.h"
#include "../arch/x86_64/spinlock.h"
#include "config.h"
#include "drivers/char/serial.h"
#include "lib/printf.h"
#include <stdarg.h>

static spinlock_t g_log_lock = SPINLOCK_INIT;

static void log_putchar(char c, void *ctx) {
    (void) ctx;
    serial_putchar(COM1, c);
}

static int g_klog_level =
#ifdef CONFIG_LOG_LEVEL
    CONFIG_LOG_LEVEL
#else
    KLOG_WARN
#endif
    ;

void klog_set_level(int level) { g_klog_level = level; }
int klog_get_level(void) { return g_klog_level; }

void klog_printf(int level, const char *fmt, ...) {
    if (level > g_klog_level) return;

    spin_lock(&g_log_lock);
    va_list ap;
    va_start(ap, fmt);
    vprintf_cb(log_putchar, NULL, fmt, ap);
    va_end(ap);
    spin_unlock(&g_log_lock);
}
