#include "types.h"
#include "vga.h"
#include "keyboard.h"
#include "gdt.h"
#include "shell.h"

static uint16_t* const VGA_MEMORY = (uint16_t*)0xB8000;
static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;

const char* HEADER[] = {
    "    AAAA    N   N  TTTTT  H   H  RRRR   OOO  DDDD   RRRR  ",
    "   A    A   NN  N    T    H   H  R   R O   O D   D  R   R ",
    "   AAAAAA   N N N    T    HHHHH  RRRR  O   O D   D  RRRR  ",
    "   A    A   N  NN    T    H   H  R  R  O   O D   D  R  R  ",
    "   A    A   N   N    T    H   H  R   R  OOO  DDDD   R   R ",
    "",
    "                Anthrodr - KFS-2 (GDT & Stack)              ",
    "",
    NULL
};

/* Make terminal_putchar visible to other files */
void terminal_putchar(char c);

void terminal_initialize(void) 
{
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* Clear the screen */
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            VGA_MEMORY[index] = vga_entry(' ', terminal_color);
        }
    }
}

static void terminal_putchar_at(char c, uint8_t color, size_t x, size_t y) 
{
    const size_t index = y * VGA_WIDTH + x;
    VGA_MEMORY[index] = vga_entry(c, color);
}

static size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len])
        len++;
    return len;
}

void terminal_putchar(char c) 
{
    if (c == '\n') {
        terminal_column = 0;
        terminal_row++;
        if (terminal_row == VGA_HEIGHT)
            terminal_row = 0;
        return;
    }

    terminal_putchar_at(c, terminal_color, terminal_column, terminal_row);
    
    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_row = 0;
    }
}

void terminal_write(const char* str) 
{
    for (size_t i = 0; str[i] != '\0'; i++)
        terminal_putchar(str[i]);
}

static void terminal_writestr_centered(const char* str, size_t row) 
{
    size_t len = strlen(str);
    size_t col = (VGA_WIDTH - len) / 2;
    
    uint8_t colors[] = {
        vga_entry_color(VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK),
        vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK),
        vga_entry_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK),
        vga_entry_color(VGA_COLOR_BLUE, VGA_COLOR_BLACK)
    };

    for (size_t i = 0; str[i]; i++) {
        size_t color_index = (i + row) % 4;
        terminal_putchar_at(str[i], colors[color_index], col + i, row);
    }
}

static void print_header(void)
{
    for (size_t i = 0; HEADER[i] != NULL; i++) {
        terminal_writestr_centered(HEADER[i], i);
    }
    terminal_row = 10;
    terminal_column = 0;
}

/* Utility: Print a 32-bit number in hexadecimal */
static void print_hex(uint32_t num) {
    char hex_chars[] = "0123456789ABCDEF";
    char str[9];
    str[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        str[i] = hex_chars[num & 0xF];
        num >>= 4;
    }
    terminal_write(str);
}

/* Print the current kernel stack pointer (ESP) */
static void print_kernel_stack(void)
{
    uint32_t esp;
    
    // Retrieve current ESP with a memory clobber
    asm volatile("movl %%esp, %0" : "=r"(esp) : : "memory");

    terminal_write("\nKernel stack pointer (ESP): 0x");
    print_hex(esp);
    terminal_write("\n");
    
}


void kernel_main(void) 
{
    terminal_initialize();
    
    /* Initialize the Global Descriptor Table */
    init_gdt();

    print_header();
    
    terminal_color = vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    
    terminal_write("\nWelcome to KFS-2!\n");
    terminal_write("42 School Kernel From Scratch - v2.0\n\n");
    terminal_write("Printing kernel stack info:\n");
    print_kernel_stack();

    terminal_write("Type your commands below.\n\n");

    /* Initialize the keyboard (if additional initialization is needed) */
    init_keyboard();

    /* Launch the shell */
    shell_run();

    // After exiting the shell, halt the CPU.
    while (1) {
        asm volatile("hlt");
    }
}