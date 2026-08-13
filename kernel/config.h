/* Kyronix kernel build configuration.
 *
 * 1 means feature is on. In the code, checking like: #ifdef CONFIG_*,
 * so for turn off just comment the string.
 *
 *   CONFIG_SERIAL_CONSOLE  1  serial console (COM1)
 *   CONFIG_LOG_LEVEL       1  the kernel logs level (see lib/log.c)
 *   CONFIG_KMEMLEAK        1  /proc/kmemleak, tool for kernel memory leak trace
 *   CONFIG_PROFILER        1  /proc/profile, profiler (250 Hz)
 *
 * Notes: backtraces kmemleak/profiler builds via frame pointers, so for
 *    full picture should build with INSTRUMENT=1 (-fno-omit-frame-pointer).
 *    By default, kernel builts with -fomit-frame-pointer,
 *    and backtraces depths will not full.
 */

#define CONFIG_SERIAL_CONSOLE 1
#define CONFIG_LOG_LEVEL 1
#define CONFIG_KMEMLEAK 1
#define CONFIG_PROFILER 1
