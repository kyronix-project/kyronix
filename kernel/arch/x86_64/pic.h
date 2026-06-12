#pragma once
#include "cpu.h"

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

static inline void pic_remap(uint8_t off1, uint8_t off2)
{
    uint8_t m1 = inb(PIC1_DATA);
    uint8_t m2 = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11);
    io_wait(); /* ICW1: init + ICW4 needed */
    outb(PIC2_CMD, 0x11);
    io_wait();
    outb(PIC1_DATA, off1);
    io_wait(); /* ICW2: master vector base */
    outb(PIC2_DATA, off2);
    io_wait(); /* ICW2: slave vector base */
    outb(PIC1_DATA, 4);
    io_wait(); /* ICW3: slave on master IRQ2 */
    outb(PIC2_DATA, 2);
    io_wait(); /* ICW3: slave cascade identity */
    outb(PIC1_DATA, 0x01);
    io_wait(); /* ICW4: 8086 mode */
    outb(PIC2_DATA, 0x01);
    io_wait();

    outb(PIC1_DATA, m1);
    outb(PIC2_DATA, m2);
}

static inline void pic_mask_all(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

static inline void pic_mask_irq(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (irq < 8) ? irq : (uint8_t) (irq - 8);
    outb(port, inb(port) | (uint8_t) (1u << bit));
}

static inline void pic_unmask_irq(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8)
    {
        irq -= 8;
        /* slave IRQs reach the CPU only if the cascade line (IRQ2) is unmasked */
        outb(PIC1_DATA, inb(PIC1_DATA) & (uint8_t) ~(1u << 2));
    }
    outb(port, inb(port) & (uint8_t) ~(1u << irq));
}

static inline void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}
