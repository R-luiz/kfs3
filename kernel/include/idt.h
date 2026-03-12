#ifndef IDT_H
#define IDT_H

#include "types.h"
#include "io.h"

#define IDT_ENTRY_COUNT      256
#define IRQ_BASE_VECTOR      32
#define IRQ0_VECTOR          32
#define IRQ1_VECTOR          33
#define IRQ8_VECTOR          40
#define SYSCALL_VECTOR       0x80

#define IDT_FLAG_INTERRUPT_GATE  0x8E
#define IDT_FLAG_SYSCALL_GATE    0xEE

struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

typedef struct cpu_registers {
    uint32_t gs;
    uint32_t fs;
    uint32_t es;
    uint32_t ds;
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t int_no;
    uint32_t err_code;
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
} cpu_registers_t;

typedef void (*interrupt_handler_t)(cpu_registers_t *registers);

void idt_init(void);
void idt_register_handler(uint8_t vector, interrupt_handler_t handler);
void idt_unregister_handler(uint8_t vector);
void idt_get_descriptor(uint32_t *base, uint16_t *limit);
void idt_handle_interrupt(cpu_registers_t *registers);

void pic_remap(uint8_t master_offset, uint8_t slave_offset);
void pic_send_eoi(uint8_t irq);
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);

static inline void idt_enable_interrupts(void)
{
    interrupts_enable();
}

static inline void idt_disable_interrupts(void)
{
    interrupts_disable();
}

static inline int idt_interrupts_enabled(void)
{
    return (read_eflags() & 0x200U) != 0;
}

#endif /* IDT_H */