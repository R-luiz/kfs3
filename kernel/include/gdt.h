#ifndef GDT_H
#define GDT_H

#include "types.h"

#define GDT_BASE_ADDRESS           0x00000800
#define GDT_ENTRY_COUNT            7

#define GDT_KERNEL_CODE_SELECTOR   0x08
#define GDT_KERNEL_DATA_SELECTOR   0x10
#define GDT_KERNEL_STACK_SELECTOR  0x18
#define GDT_USER_CODE_SELECTOR     0x20
#define GDT_USER_DATA_SELECTOR     0x28
#define GDT_USER_STACK_SELECTOR    0x30

/* A GDT entry is 8 bytes */
struct gdt_entry {
    uint16_t limit_low;     // Lower 16 bits of limit
    uint16_t base_low;      // Lower 16 bits of base address
    uint8_t base_middle;    // Next 8 bits of base address
    uint8_t access;         // Access flags
    uint8_t granularity;    // Granularity and high order 4 bits of limit
    uint8_t base_high;      // Highest 8 bits of base address
} __attribute__((packed));

/* Pointer structure to pass to LGDT */
struct gdt_ptr {
    uint16_t limit;         // Limit of the GDT
    uint32_t base;          // Base address of the first gdt_entry
} __attribute__((packed));

/* Initialize and load the GDT */
void init_gdt(void);

#endif /* GDT_H */
