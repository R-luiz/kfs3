#include "panic.h"
#include "serial.h"
#include "vga.h"

/* External VGA functions from main.c */
extern void terminal_write(const char *str);
extern void terminal_putchar(char c);

/* VGA direct access for panic screen */
static uint16_t* const PANIC_VGA = (uint16_t*)0xB8000;
static const size_t PANIC_WIDTH = 80;
static const size_t PANIC_HEIGHT = 25;

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

/* Fatal kernel panic */
void panic(const char *message)
{
    /* Disable interrupts */
    asm volatile("cli");

    /* Clear screen with red background */
    panic_clear_screen(VGA_COLOR_RED);

    uint8_t title_color = vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
    uint8_t msg_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_RED);

    /* Print panic header */
    panic_print("================================================================================", 0, title_color);
    panic_print("                            KERNEL PANIC                                        ", 1, title_color);
    panic_print("================================================================================", 2, title_color);

    /* Print message */
    panic_print("Error: ", 4, msg_color);
    panic_print(message, 5, title_color);

    /* Print instructions */
    panic_print("The system has been halted.", 8, msg_color);
    panic_print("Please restart your computer.", 10, msg_color);

    serial_write("\n\nKERNEL PANIC\nError: ");
    serial_write(message);
    serial_write("\nThe system has been halted.\nPlease restart your computer.\n");

    /* Halt the CPU */
    while (1) {
        asm volatile("hlt");
    }
}

/* Panic with file and line information */
void panic_at(const char *message, const char *file, int line)
{
    /* Disable interrupts */
    asm volatile("cli");

    /* Clear screen with red background */
    panic_clear_screen(VGA_COLOR_RED);

    uint8_t title_color = vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
    uint8_t msg_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_RED);
    uint8_t loc_color = vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_RED);

    /* Print panic header */
    panic_print("================================================================================", 0, title_color);
    panic_print("                            KERNEL PANIC                                        ", 1, title_color);
    panic_print("================================================================================", 2, title_color);

    /* Print message */
    panic_print("Error: ", 4, msg_color);
    panic_print(message, 5, title_color);

    /* Print location */
    panic_print("Location: ", 7, msg_color);
    panic_print(file, 8, loc_color);
    panic_print("Line: ", 9, msg_color);
    panic_print_int(line, 9, 6, loc_color);

    /* Print instructions */
    panic_print("The system has been halted.", 12, msg_color);
    panic_print("Please restart your computer.", 14, msg_color);

    serial_write("\n\nKERNEL PANIC\nError: ");
    serial_write(message);
    serial_write("\nLocation: ");
    serial_write(file);
    serial_write("\nLine: ");
    panic_serial_print_int(line);
    serial_write("\nThe system has been halted.\nPlease restart your computer.\n");

    /* Halt the CPU */
    while (1) {
        asm volatile("hlt");
    }
}

/* Non-fatal warning */
void warn(const char *message)
{
    /* Print warning prefix */
    terminal_write("[WARNING] ");
    terminal_write(message);
    terminal_putchar('\n');
}
