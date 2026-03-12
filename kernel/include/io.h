#ifndef IO_H
#define IO_H

#include "types.h"

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    asm volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value)
{
    asm volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline void io_wait(void)
{
    asm volatile ("outb %%al, $0x80" :: "a"(0));
}

static inline void interrupts_enable(void)
{
    asm volatile ("sti" ::: "memory");
}

static inline void interrupts_disable(void)
{
    asm volatile ("cli" ::: "memory");
}

static inline void cpu_halt(void)
{
    asm volatile ("hlt");
}

static inline uint32_t read_eflags(void)
{
    uint32_t flags;
    asm volatile ("pushfl; popl %0" : "=r"(flags) :: "memory");
    return flags;
}

static inline uint32_t interrupts_save(void)
{
    uint32_t flags = read_eflags();
    interrupts_disable();
    return flags;
}

static inline void interrupts_restore(uint32_t flags)
{
    asm volatile ("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
}

#endif /* IO_H */