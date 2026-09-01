#pragma once

#include <stdbool.h>
#include <stdint.h>

#define COM1 0x3F8
#define COM2 0x2F8

#ifdef CONFIG_SERIAL_CONSOLE
bool serial_init(uint16_t port);
void serial_putchar(uint16_t port, char c);
void serial_write(uint16_t port, const char *s);
void serial_write_n(uint16_t port, const char *s, uint64_t len);

bool serial_data_ready(uint16_t port);

uint8_t serial_getchar(uint16_t port);
#else
static inline bool serial_init(uint16_t port) {
    (void) port;
    return false;
}
static inline void serial_putchar(uint16_t port, char c) {
    (void) port;
    (void) c;
}
static inline void serial_write(uint16_t port, const char *s) {
    (void) port;
    (void) s;
}
static inline void serial_write_n(uint16_t port, const char *s, uint64_t len) {
    (void) port;
    (void) s;
    (void) len;
}
static inline bool serial_data_ready(uint16_t port) {
    (void) port;
    return false;
}
static inline uint8_t serial_getchar(uint16_t port) {
    (void) port;
    return 0;
}
#endif
