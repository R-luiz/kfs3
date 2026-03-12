#include "idt.h"
#include "gdt.h"
#include "panic.h"

#define PIC1_COMMAND_PORT    0x20
#define PIC1_DATA_PORT       0x21
#define PIC2_COMMAND_PORT    0xA0
#define PIC2_DATA_PORT       0xA1

#define PIC_ICW1_INIT        0x10
#define PIC_ICW1_ICW4        0x01
#define PIC_ICW4_8086        0x01
#define PIC_EOI              0x20

extern void *interrupt_stub_table[];
extern void isr128(void);

static struct idt_entry idt_entries[IDT_ENTRY_COUNT];
static struct idt_ptr idt_descriptor;
static interrupt_handler_t interrupt_handlers[IDT_ENTRY_COUNT];

static const char *exception_messages[32] = {
    "Division by zero",
    "Debugger",
    "Non-maskable interrupt",
    "Breakpoint",
    "Overflow",
    "Bound range exceeded",
    "Invalid opcode",
    "Device not available",
    "Double fault",
    "Coprocessor segment overrun",
    "Invalid task state segment",
    "Segment not present",
    "Stack fault",
    "General protection fault",
    "Page fault",
    "Reserved exception",
    "Math fault",
    "Alignment check",
    "Machine check",
    "SIMD floating-point exception",
    "Reserved exception",
    "Reserved exception",
    "Reserved exception",
    "Reserved exception",
    "Reserved exception",
    "Reserved exception",
    "Reserved exception",
    "Reserved exception",
    "Reserved exception",
    "Reserved exception",
    "Reserved exception",
    "Reserved exception",
};

static void idt_set_gate(uint8_t vector, uint32_t base, uint16_t selector, uint8_t flags)
{
    idt_entries[vector].base_low = (uint16_t)(base & 0xFFFF);
    idt_entries[vector].selector = selector;
    idt_entries[vector].zero = 0;
    idt_entries[vector].flags = flags;
    idt_entries[vector].base_high = (uint16_t)((base >> 16) & 0xFFFF);
}

static void idt_clear_tables(void)
{
    uint32_t index;

    for (index = 0; index < IDT_ENTRY_COUNT; index++) {
        idt_entries[index].base_low = 0;
        idt_entries[index].selector = 0;
        idt_entries[index].zero = 0;
        idt_entries[index].flags = 0;
        idt_entries[index].base_high = 0;
        interrupt_handlers[index] = NULL;
    }
}

void pic_remap(uint8_t master_offset, uint8_t slave_offset)
{
    uint8_t master_mask = inb(PIC1_DATA_PORT);
    uint8_t slave_mask = inb(PIC2_DATA_PORT);

    outb(PIC1_COMMAND_PORT, PIC_ICW1_INIT | PIC_ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND_PORT, PIC_ICW1_INIT | PIC_ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA_PORT, master_offset);
    io_wait();
    outb(PIC2_DATA_PORT, slave_offset);
    io_wait();

    outb(PIC1_DATA_PORT, 4);
    io_wait();
    outb(PIC2_DATA_PORT, 2);
    io_wait();

    outb(PIC1_DATA_PORT, PIC_ICW4_8086);
    io_wait();
    outb(PIC2_DATA_PORT, PIC_ICW4_8086);
    io_wait();

    outb(PIC1_DATA_PORT, master_mask);
    outb(PIC2_DATA_PORT, slave_mask);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_COMMAND_PORT, PIC_EOI);
    }
    outb(PIC1_COMMAND_PORT, PIC_EOI);
}

void pic_set_mask(uint8_t irq)
{
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA_PORT;
    } else {
        port = PIC2_DATA_PORT;
        irq -= 8;
    }

    value = inb(port) | (uint8_t)(1U << irq);
    outb(port, value);
}

void pic_clear_mask(uint8_t irq)
{
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA_PORT;
    } else {
        port = PIC2_DATA_PORT;
        irq -= 8;
    }

    value = inb(port) & (uint8_t)~(1U << irq);
    outb(port, value);
}

void idt_init(void)
{
    uint32_t vector;

    idt_clear_tables();

    for (vector = 0; vector < 48; vector++) {
        idt_set_gate((uint8_t)vector, (uint32_t)interrupt_stub_table[vector], GDT_KERNEL_CODE_SELECTOR, IDT_FLAG_INTERRUPT_GATE);
    }
    idt_set_gate(SYSCALL_VECTOR, (uint32_t)isr128, GDT_KERNEL_CODE_SELECTOR, IDT_FLAG_SYSCALL_GATE);

    idt_descriptor.limit = (uint16_t)(sizeof(idt_entries) - 1);
    idt_descriptor.base = (uint32_t)&idt_entries;

    pic_remap(IRQ_BASE_VECTOR, IRQ_BASE_VECTOR + 8);
    outb(PIC1_DATA_PORT, 0xF9);
    outb(PIC2_DATA_PORT, 0xFF);

    asm volatile ("lidt (%0)" :: "r"(&idt_descriptor) : "memory");
}

void idt_register_handler(uint8_t vector, interrupt_handler_t handler)
{
    interrupt_handlers[vector] = handler;
}

void idt_unregister_handler(uint8_t vector)
{
    interrupt_handlers[vector] = NULL;
}

void idt_get_descriptor(uint32_t *base, uint16_t *limit)
{
    if (base != NULL) {
        *base = idt_descriptor.base;
    }
    if (limit != NULL) {
        *limit = idt_descriptor.limit;
    }
}

void idt_handle_interrupt(cpu_registers_t *registers)
{
    uint32_t vector;

    if (registers == NULL) {
        return;
    }

    vector = registers->int_no & 0xFFU;

    if (interrupt_handlers[vector] != NULL) {
        interrupt_handlers[vector](registers);
    } else if (vector < 32) {
        panic_with_registers(exception_messages[vector], registers);
    }

    if (vector >= IRQ_BASE_VECTOR && vector < IRQ_BASE_VECTOR + 16) {
        pic_send_eoi((uint8_t)(vector - IRQ_BASE_VECTOR));
    }
}