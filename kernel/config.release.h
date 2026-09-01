/* Kyronix kernel build configuration — RELEASE variant.
 *
 * Same layout as kernel/config.h, but all debug/observability features
 * are disabled for production images:
 *
 *   CONFIG_SERIAL_CONSOLE  serial console (COM1)          OFF
 *   CONFIG_LOG_LEVEL       kernel log level               OFF
 *   CONFIG_KMEMLEAK        /proc/kmemleak                 OFF
 *   CONFIG_PROFILER        /proc/profile                  OFF
 *
 * All kernel sources are wrapped in #ifdef CONFIG_*, so disabling the
 * macro both drops the code and (via inline stubs in serial.h) turns the
 * serial driver and its callers (klog/kdbg/tty) into no-ops.
 *
 * Feature macros below are left undefined — nothing to enable.
 */
