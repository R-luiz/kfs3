#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"
#include "vga.h"

// Keyboard I/O ports
#define KEYBOARD_DATA_PORT    0x60
#define KEYBOARD_STATUS_PORT  0x64

// Function declarations
void init_keyboard(void);
void keyboard_handler(void);

// Read from I/O port
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Write to I/O port
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

#endif /* KEYBOARD_H */