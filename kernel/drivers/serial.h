#pragma once

#include <stdbool.h>
#include <stdint.h>

#define COM1 0x3F8
#define COM2 0x2F8

bool serial_init(uint16_t port);
void serial_putchar(uint16_t port, char c);
void serial_write(uint16_t port, const char *s);

bool serial_data_ready(uint16_t port);

uint8_t serial_getchar(uint16_t port);
