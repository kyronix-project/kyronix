#include "serial.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/spinlock.h"
#include "../lib/string.h"

static spinlock_t g_serial_lock = SPINLOCK_INIT;

#define UART_DATA 0
#define UART_IER 1
#define UART_FCR 2
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5
#define UART_DLL 0
#define UART_DLH 1

#define LSR_DR (1 << 0)
#define LSR_THRE (1 << 5)
#define LCR_DLAB (1 << 7)

bool serial_init(uint16_t port) {
    outb(port + UART_IER, 0x00);
    outb(port + UART_LCR, LCR_DLAB);
    outb(port + UART_DLL, 0x03);
    outb(port + UART_DLH, 0x00);
    outb(port + UART_LCR, 0x03);
    outb(port + UART_FCR, 0xC7);
    outb(port + UART_MCR, 0x0B);

    outb(port + UART_MCR, 0x1E);
    outb(port + UART_DATA, 0xAE);
    if (inb(port + UART_DATA) != 0xAE) return false;

    outb(port + UART_MCR, 0x0F);
    return true;
}

void serial_putchar(uint16_t port, char c) {
    spin_lock(&g_serial_lock);
    while (!(inb(port + UART_LSR) & LSR_THRE)) cpu_relax();
    outb(port + UART_DATA, (uint8_t) c);
    spin_unlock(&g_serial_lock);
}

void serial_write(uint16_t port, const char *s) {
    while (*s) serial_putchar(port, *s++);
}

bool serial_data_ready(uint16_t port) { return (inb(port + UART_LSR) & LSR_DR) != 0; }

uint8_t serial_getchar(uint16_t port) {
    while (!serial_data_ready(port)) cpu_relax();
    return inb(port + UART_DATA);
}