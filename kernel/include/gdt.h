#ifndef GDT_H
#define GDT_H

#include "types.h"

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

/* We place our GDT in its own section so the linker puts it at 0x800 */
extern struct gdt_entry gdt_entries[];
extern struct gdt_ptr gdt_ptr;

/* Initialize and load the GDT */
void init_gdt(void);

#endif /* GDT_H */
