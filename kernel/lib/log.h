#pragma once

void klog_set_level(int level);
void klog_printf(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define KLOG_ERROR 0
#define KLOG_WARN 1
#define KLOG_INFO 2
#define KLOG_DEBUG 3

#define log_info(fmt, ...) klog_printf(KLOG_INFO, "I: " fmt "\n", ##__VA_ARGS__)
#define log_warn(fmt, ...) klog_printf(KLOG_WARN, "W: " fmt "\n", ##__VA_ARGS__)
#define log_error(fmt, ...) klog_printf(KLOG_ERROR, "E: " fmt "\n", ##__VA_ARGS__)
#define log_debug(fmt, ...) klog_printf(KLOG_DEBUG, "D: " fmt "\n", ##__VA_ARGS__)
