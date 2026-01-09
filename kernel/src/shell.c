#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "types.h"

#define SHELL_BUFFER_SIZE 256

// Simple scancode-to-ASCII mapping (same as before)
static const char scancode_to_ascii[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0,
    0, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

/* External terminal routines that are now public (see main.c modifications) */
extern void terminal_putchar(char c);
extern void terminal_write(const char* str);
extern void terminal_initialize(void);

/* A few very basic string helper functions */
static int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static int strncmp(const char *s1, const char *s2, size_t n) {
    while(n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if(n == 0)
        return 0;
    else
        return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

/*
 * shell_getchar: busy-polls the keyboard until a key press is detected.
 */
static char shell_getchar(void) {
    char c = 0;
    while (1) {
        if (inb(KEYBOARD_STATUS_PORT) & 0x01) {
            uint8_t scancode = inb(KEYBOARD_DATA_PORT);
            // Only process key press events (ignore releases)
            if (scancode < sizeof(scancode_to_ascii) && !(scancode & 0x80)) {
                c = scancode_to_ascii[scancode];
                // Wait until the key is released to avoid autorepeat issues.
                while (inb(KEYBOARD_STATUS_PORT) & 0x01)
                    ;
                break;
            }
        }
    }
    return c;
}

/*
 * shell_run: Implements a simple read-evaluate loop supporting the commands:
 *  - help:   Display a help message.
 *  - echo:   Print back the text following the command.
 *  - clear:  Clear the screen.
 *  - ls:     List files (a hard-coded file list).
 *  - exit:   Exit the shell.
 */
void shell_run(void) {
    char buffer[SHELL_BUFFER_SIZE];
    size_t pos = 0;

    while (1) {
        /* Print the prompt */
        terminal_write("shell> ");
        pos = 0;
        
        /* Read a line of input */
        while (1) {
            char c = shell_getchar();

            /* Handle backspace */
            if (c == 0x08 || c == '\b') {
                if (pos > 0) {
                    pos--;
                    terminal_putchar('\b');
                    terminal_putchar(' ');
                    terminal_putchar('\b');
                }
                continue;
            }
            
            /* Handle ENTER (newline) */
            if (c == '\n') {
                terminal_putchar('\n');
                break;
            }

            /* Echo the character and store it if there's room */
            if (pos < SHELL_BUFFER_SIZE - 1) {
                buffer[pos++] = c;
                terminal_putchar(c);
            }
        }
        buffer[pos] = '\0';  // NULL-terminate the command

        /* Process the input command */
        if (strcmp(buffer, "help") == 0) {
            terminal_write("Built-in commands:\n");
            terminal_write("  help      - Show this help message\n");
            terminal_write("  echo TEXT - Print TEXT\n");
            terminal_write("  clear     - Clear the screen\n");
            terminal_write("  ls        - List files (simulated)\n");
            terminal_write("  exit      - Exit the shell\n");
        } else if (strncmp(buffer, "echo ", 5) == 0) {
            terminal_write(buffer + 5);
            terminal_putchar('\n');
        }
        else if (strcmp(buffer, "clear") == 0) {
            // Clear the screen using terminal_initialize
            terminal_initialize();
        }
        else if (strcmp(buffer, "ls") == 0) {
            // Simulate a file/directory listing. Adjust entries as needed.
            terminal_write("boot/\n");
            terminal_write("kernel/\n");
            terminal_write("tools/\n");
            terminal_write("README.md\n");
        } else if (strcmp(buffer, "exit") == 0) {
            terminal_write("Exiting shell...\n");
            break;
        } else if (buffer[0] != '\0') {
            terminal_write("Command not found\n");
        }
    }
}
