#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NORETURN __attribute__((noreturn))
#define PACKED __attribute__((packed))
#define ALIGNED(n) __attribute__((aligned(n)))
#define INLINE static inline __attribute__((always_inline))
#define UNUSED __attribute__((unused))

INLINE void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

INLINE void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

INLINE uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

INLINE uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

INLINE void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

INLINE uint32_t inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

INLINE void io_wait(void)
{
    outb(0x80, 0);
}

INLINE void cli(void)
{
    __asm__ volatile("cli" ::: "memory");
}
INLINE void sti(void)
{
    __asm__ volatile("sti" ::: "memory");
}
INLINE void hlt(void)
{
    __asm__ volatile("hlt" ::: "memory");
}

INLINE void cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

NORETURN INLINE void cpu_halt(void)
{
    cli();
    for (;;)
        hlt();
}

INLINE uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) : "memory");
    return ((uint64_t) hi << 32) | lo;
}

INLINE void wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile("wrmsr" ::"c"(msr), "a"((uint32_t) val), "d"((uint32_t) (val >> 32))
                     : "memory");
}

INLINE uint64_t read_cr0(void)
{
    uint64_t val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

INLINE uint64_t read_cr2(void)
{
    uint64_t val;
    __asm__ volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

INLINE uint64_t read_cr3(void)
{
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

INLINE void write_cr3(uint64_t val)
{
    __asm__ volatile("mov %0, %%cr3" ::"r"(val) : "memory");
}

INLINE void write_cr0(uint64_t val)
{
    __asm__ volatile("mov %0, %%cr0" ::"r"(val) : "memory");
}

INLINE uint64_t read_cr4(void)
{
    uint64_t val;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}

INLINE void write_cr4(uint64_t val)
{
    __asm__ volatile("mov %0, %%cr4" ::"r"(val) : "memory");
}

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} PACKED gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} PACKED gdtr_t;

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} PACKED idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} PACKED idtr_t;

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} PACKED cpu_state_t;
