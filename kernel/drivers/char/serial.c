#include "serial.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/spinlock.h"
#include "../lib/string.h"

#ifdef CONFIG_SERIAL_CONSOLE

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
#define UART_POLL_LIMIT 100000u

static bool g_ready[2];

static int port_index(uint16_t port) { return port == COM1 ? 0 : port == COM2 ? 1 : -1; }

static bool tx_ready(uint16_t port, int index) {
    for (unsigned i = 0; i < UART_POLL_LIMIT; i++) {
        uint8_t status = inb(port + UART_LSR);
        if (status == 0xff) break;
        if (status & LSR_THRE) return true;
        cpu_relax();
    }
    g_ready[index] = false;
    return false;
}

bool serial_init(uint16_t port) {
    int index = port_index(port);
    if (index < 0) return false;
    g_ready[index] = false;
    outb(port + UART_IER, 0x00);
    outb(port + UART_LCR, LCR_DLAB);
    outb(port + UART_DLL, 0x03);
    outb(port + UART_DLH, 0x00);
    outb(port + UART_LCR, 0x03);
    outb(port + UART_FCR, 0xC7);
    outb(port + UART_MCR, 0x0B);

    outb(port + UART_MCR, 0x1E);
    outb(port + UART_DATA, 0xAE);
    bool ok = inb(port + UART_DATA) == 0xAE;
    outb(port + UART_MCR, 0x0F);
    g_ready[index] = ok;
    return ok;
}

void serial_putchar(uint16_t port, char c) {
    serial_write_n(port, &c, 1);
}

void serial_write(uint16_t port, const char *s) {
    serial_write_n(port, s, strlen(s));
}

void serial_write_n(uint16_t port, const char *s, uint64_t len) {
    int index = port_index(port);
    if (index < 0) return;
    uint64_t flags = irq_save();
    spin_lock(&g_serial_lock);
    while (g_ready[index] && len && tx_ready(port, index)) {
        outb(port + UART_DATA, (uint8_t) *s++);
        len--;
    }
    spin_unlock(&g_serial_lock);
    irq_restore(flags);
}

bool serial_data_ready(uint16_t port) {
    int index = port_index(port);
    if (index < 0 || !g_ready[index]) return false;
    uint8_t status = inb(port + UART_LSR);
    return status != 0xff && (status & LSR_DR) != 0;
}

uint8_t serial_getchar(uint16_t port) {
    for (unsigned i = 0; i < UART_POLL_LIMIT; i++) {
        if (serial_data_ready(port)) return inb(port + UART_DATA);
        cpu_relax();
    }
    return 0;
}

#endif /* CONFIG_SERIAL_CONSOLE */
