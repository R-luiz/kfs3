#include "keyboard.h"

// Simple scancode to ASCII mapping
static const char scancode_to_ascii[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0,
    0, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

// External function to write character (defined in main.c)
extern void terminal_putchar(char c);

void keyboard_handler(void) {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    
    // Only handle key press events (ignore key release)
    if (scancode < sizeof(scancode_to_ascii) && !(scancode & 0x80)) {
        char c = scancode_to_ascii[scancode];
        if (c != 0) {
            terminal_putchar(c);
        }
    }
}

void init_keyboard(void) {
    // Basic PS/2 keyboard initialization
    // Read and discard any pending keyboard data
    while (inb(KEYBOARD_STATUS_PORT) & 1) {
        inb(KEYBOARD_DATA_PORT);
    }
}