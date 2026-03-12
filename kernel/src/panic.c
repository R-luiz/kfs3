#include "panic.h"
#include "serial.h"
#include "signals.h"
#include "vga.h"
#include "io.h"

/* External VGA functions from main.c */
extern void terminal_write(const char *str);
extern void terminal_putchar(char c);
extern uint8_t stack_bottom;
extern uint8_t stack_top;

/* VGA direct access for panic screen */
static uint16_t* const PANIC_VGA = (uint16_t*)0xB8000;
static const size_t PANIC_WIDTH = 80;
static const size_t PANIC_HEIGHT = 25;
static panic_context_t last_panic_context;

/* Print a string directly to VGA with color (bypasses terminal state) */
static void panic_print(const char *str, size_t row, uint8_t color)
{
    size_t col = 0;
    for (size_t i = 0; str[i] != '\0' && col < PANIC_WIDTH; i++) {
        if (str[i] == '\n') {
            row++;
            col = 0;
            continue;
        }
        size_t index = row * PANIC_WIDTH + col;
        PANIC_VGA[index] = vga_entry(str[i], color);
        col++;
    }
}

static void panic_print_hex(size_t row, size_t col, uint32_t value, uint8_t color)
{
    static const char hex_chars[] = "0123456789ABCDEF";
    size_t index = row * PANIC_WIDTH + col;
    int shift;

    PANIC_VGA[index++] = vga_entry('0', color);
    PANIC_VGA[index++] = vga_entry('x', color);
    for (shift = 28; shift >= 0; shift -= 4) {
        PANIC_VGA[index++] = vga_entry(hex_chars[(value >> shift) & 0x0F], color);
    }
}

/* Clear screen with a specific background color */
static void panic_clear_screen(uint8_t bg_color)
{
    uint8_t color = vga_entry_color(VGA_COLOR_WHITE, bg_color);
    for (size_t i = 0; i < PANIC_WIDTH * PANIC_HEIGHT; i++) {
        PANIC_VGA[i] = vga_entry(' ', color);
    }
}

/* Print integer as decimal */
static void panic_print_int(int num, size_t row, size_t col, uint8_t color)
{
    char buf[12];
    int i = 0;
    int neg = 0;

    if (num < 0) {
        neg = 1;
        num = -num;
    }

    if (num == 0) {
        buf[i++] = '0';
    } else {
        while (num > 0) {
            buf[i++] = '0' + (num % 10);
            num /= 10;
        }
    }

    if (neg) buf[i++] = '-';

    /* Print in reverse */
    size_t idx = row * PANIC_WIDTH + col;
    while (i > 0) {
        PANIC_VGA[idx++] = vga_entry(buf[--i], color);
    }
}

static void panic_serial_print_int(int num)
{
    char buf[12];
    int i = 0;
    int neg = 0;

    if (num < 0) {
        neg = 1;
        num = -num;
    }

    if (num == 0) {
        serial_write_char('0');
        return;
    }

    while (num > 0) {
        buf[i++] = '0' + (num % 10);
        num /= 10;
    }

    if (neg) {
        buf[i++] = '-';
    }

    while (i > 0) {
        serial_write_char(buf[--i]);
    }
}

static void panic_serial_print_hex(uint32_t value)
{
    static const char hex_chars[] = "0123456789ABCDEF";
    int shift;

    serial_write("0x");
    for (shift = 28; shift >= 0; shift -= 4) {
        serial_write_char(hex_chars[(value >> shift) & 0x0F]);
    }
}

static uint32_t panic_capture_eip(void)
{
    uint32_t eip;
    asm volatile (
        "call 1f\n"
        "1: pop %0\n"
        : "=r"(eip)
    );
    return eip;
}

static void panic_reset_context(void)
{
    size_t index;

    last_panic_context.valid = 0;
    last_panic_context.has_registers = 0;
    last_panic_context.saved_stack_pointer = 0;
    last_panic_context.saved_stack_size = 0;
    last_panic_context.register_snapshot.gs = 0;
    last_panic_context.register_snapshot.fs = 0;
    last_panic_context.register_snapshot.es = 0;
    last_panic_context.register_snapshot.ds = 0;
    last_panic_context.register_snapshot.edi = 0;
    last_panic_context.register_snapshot.esi = 0;
    last_panic_context.register_snapshot.ebp = 0;
    last_panic_context.register_snapshot.esp = 0;
    last_panic_context.register_snapshot.ebx = 0;
    last_panic_context.register_snapshot.edx = 0;
    last_panic_context.register_snapshot.ecx = 0;
    last_panic_context.register_snapshot.eax = 0;
    last_panic_context.register_snapshot.int_no = 0;
    last_panic_context.register_snapshot.err_code = 0;
    last_panic_context.register_snapshot.eip = 0;
    last_panic_context.register_snapshot.cs = 0;
    last_panic_context.register_snapshot.eflags = 0;

    for (index = 0; index < sizeof(last_panic_context.stack_snapshot); index++) {
        last_panic_context.stack_snapshot[index] = 0;
    }
}

static void panic_capture_current_registers(cpu_registers_t *registers)
{
    uint16_t segment;

    asm volatile ("mov %%gs, %0" : "=rm"(segment));
    registers->gs = segment;
    asm volatile ("mov %%fs, %0" : "=rm"(segment));
    registers->fs = segment;
    asm volatile ("mov %%es, %0" : "=rm"(segment));
    registers->es = segment;
    asm volatile ("mov %%ds, %0" : "=rm"(segment));
    registers->ds = segment;

    asm volatile ("mov %%edi, %0" : "=m"(registers->edi));
    asm volatile ("mov %%esi, %0" : "=m"(registers->esi));
    asm volatile ("mov %%ebp, %0" : "=m"(registers->ebp));
    asm volatile ("mov %%esp, %0" : "=m"(registers->esp));
    asm volatile ("mov %%ebx, %0" : "=m"(registers->ebx));
    asm volatile ("mov %%edx, %0" : "=m"(registers->edx));
    asm volatile ("mov %%ecx, %0" : "=m"(registers->ecx));
    asm volatile ("mov %%eax, %0" : "=m"(registers->eax));

    registers->int_no = 0xFFFFFFFF;
    registers->err_code = 0;
    registers->eip = panic_capture_eip();
    asm volatile ("mov %%cs, %0" : "=rm"(segment));
    registers->cs = segment;
    registers->eflags = read_eflags();
}

void panic_save_stack_snapshot(uint32_t stack_pointer)
{
    uint32_t stack_low = (uint32_t)&stack_bottom;
    uint32_t stack_high = (uint32_t)&stack_top;
    uint32_t bytes_to_copy;
    uint32_t index;

    last_panic_context.saved_stack_pointer = stack_pointer;
    last_panic_context.saved_stack_size = 0;

    if (stack_pointer < stack_low || stack_pointer >= stack_high) {
        return;
    }

    bytes_to_copy = stack_high - stack_pointer;
    if (bytes_to_copy > sizeof(last_panic_context.stack_snapshot)) {
        bytes_to_copy = sizeof(last_panic_context.stack_snapshot);
    }

    for (index = 0; index < bytes_to_copy; index++) {
        last_panic_context.stack_snapshot[index] = ((uint8_t *)stack_pointer)[index];
    }
    last_panic_context.saved_stack_size = bytes_to_copy;
}

void panic_save_context(const cpu_registers_t *registers)
{
    panic_reset_context();

    if (registers != NULL) {
        last_panic_context.register_snapshot = *registers;
    } else {
        panic_capture_current_registers(&last_panic_context.register_snapshot);
    }

    last_panic_context.valid = 1;
    last_panic_context.has_registers = 1;
    panic_save_stack_snapshot(last_panic_context.register_snapshot.esp);
}

const panic_context_t *panic_get_last_context(void)
{
    return &last_panic_context;
}

static void panic_render_context(const char *message, const char *file, int line)
{
    uint8_t title_color = vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
    uint8_t msg_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_RED);
    uint8_t loc_color = vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_RED);
    uint32_t index;

    panic_clear_screen(VGA_COLOR_RED);
    panic_print("================================================================================", 0, title_color);
    panic_print("                            KERNEL PANIC                                        ", 1, title_color);
    panic_print("================================================================================", 2, title_color);
    panic_print("Error: ", 4, msg_color);
    panic_print(message, 5, title_color);

    if (file != NULL) {
        panic_print("Location: ", 7, msg_color);
        panic_print(file, 8, loc_color);
        panic_print("Line: ", 9, msg_color);
        panic_print_int(line, 9, 6, loc_color);
    }

    if (last_panic_context.has_registers) {
        panic_print("INT:", 11, msg_color);
        panic_print_hex(11, 5, last_panic_context.register_snapshot.int_no, loc_color);
        panic_print("EIP:", 12, msg_color);
        panic_print_hex(12, 5, last_panic_context.register_snapshot.eip, loc_color);
        panic_print("ESP:", 13, msg_color);
        panic_print_hex(13, 5, last_panic_context.register_snapshot.esp, loc_color);
        panic_print("EBP:", 14, msg_color);
        panic_print_hex(14, 5, last_panic_context.register_snapshot.ebp, loc_color);
        panic_print("EAX:", 15, msg_color);
        panic_print_hex(15, 5, last_panic_context.register_snapshot.eax, loc_color);
        panic_print("EBX:", 16, msg_color);
        panic_print_hex(16, 5, last_panic_context.register_snapshot.ebx, loc_color);
    }

    panic_print("Stack snapshot bytes:", 18, msg_color);
    panic_print_int((int)last_panic_context.saved_stack_size, 18, 21, loc_color);
    panic_print("The system has been halted.", 21, msg_color);
    panic_print("Please restart your computer.", 23, msg_color);

    serial_write("\n\nKERNEL PANIC\nError: ");
    serial_write(message);
    serial_write("\n");

    if (file != NULL) {
        serial_write("Location: ");
        serial_write(file);
        serial_write("\nLine: ");
        panic_serial_print_int(line);
        serial_write("\n");
    }

    if (last_panic_context.has_registers) {
        serial_write("INT: ");
        panic_serial_print_hex(last_panic_context.register_snapshot.int_no);
        serial_write("  ERR: ");
        panic_serial_print_hex(last_panic_context.register_snapshot.err_code);
        serial_write("\nEIP: ");
        panic_serial_print_hex(last_panic_context.register_snapshot.eip);
        serial_write("  ESP: ");
        panic_serial_print_hex(last_panic_context.register_snapshot.esp);
        serial_write("  EBP: ");
        panic_serial_print_hex(last_panic_context.register_snapshot.ebp);
        serial_write("\nEAX: ");
        panic_serial_print_hex(last_panic_context.register_snapshot.eax);
        serial_write("  EBX: ");
        panic_serial_print_hex(last_panic_context.register_snapshot.ebx);
        serial_write("  ECX: ");
        panic_serial_print_hex(last_panic_context.register_snapshot.ecx);
        serial_write("  EDX: ");
        panic_serial_print_hex(last_panic_context.register_snapshot.edx);
        serial_write("\nESI: ");
        panic_serial_print_hex(last_panic_context.register_snapshot.esi);
        serial_write("  EDI: ");
        panic_serial_print_hex(last_panic_context.register_snapshot.edi);
        serial_write("  EFLAGS: ");
        panic_serial_print_hex(last_panic_context.register_snapshot.eflags);
        serial_write("\n");
    }

    serial_write("Saved stack pointer: ");
    panic_serial_print_hex(last_panic_context.saved_stack_pointer);
    serial_write("\nStack snapshot: ");
    panic_serial_print_int((int)last_panic_context.saved_stack_size);
    serial_write(" bytes\n");

    for (index = 0; index < last_panic_context.saved_stack_size; index++) {
        if (index != 0 && (index % 16) == 0) {
            serial_write("\n");
        }
        panic_serial_print_hex(last_panic_context.stack_snapshot[index]);
        serial_write_char(' ');
    }
    serial_write("\nThe system has been halted.\nPlease restart your computer.\n");
}

/* Fatal kernel panic */
void panic(const char *message)
{
    panic_with_context(message, NULL, 0, NULL);
}

/* Panic with file and line information */
void panic_at(const char *message, const char *file, int line)
{
    panic_with_context(message, file, line, NULL);
}

void panic_with_registers(const char *message, const cpu_registers_t *registers)
{
    panic_with_context(message, NULL, 0, registers);
}

void panic_with_context(const char *message, const char *file, int line, const cpu_registers_t *registers)
{
    interrupts_disable();
    panic_save_context(registers);
    schedule_signal(KERNEL_SIGNAL_PANIC, 0, NULL);
    panic_render_context(message, file, line);
    panic_clean_registers_and_halt();
}

void panic_clean_registers_and_halt(void)
{
    interrupts_disable();
    asm volatile (
        "xor %%eax, %%eax\n"
        "xor %%ebx, %%ebx\n"
        "xor %%ecx, %%ecx\n"
        "xor %%edx, %%edx\n"
        "xor %%esi, %%esi\n"
        "xor %%edi, %%edi\n"
        "xor %%ebp, %%ebp\n"
        "1:\n"
        "hlt\n"
        "jmp 1b\n"
        :
        :
        : "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "memory"
    );
    __builtin_unreachable();
}

/* Non-fatal warning */
void warn(const char *message)
{
    /* Print warning prefix */
    terminal_write("[WARNING] ");
    terminal_write(message);
    terminal_putchar('\n');
}
