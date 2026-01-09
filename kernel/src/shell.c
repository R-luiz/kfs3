#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "types.h"
#include "pmm.h"
#include "kmalloc.h"
#include "vmalloc.h"
#include "paging.h"
#include "panic.h"

#define SHELL_BUFFER_SIZE 256

// Simple scancode-to-ASCII mapping (same as before)
static const char scancode_to_ascii[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
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

/* Parse a decimal number from string */
static uint32_t parse_uint(const char *str) {
    uint32_t result = 0;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result;
}

/* Parse a hex number from string (with or without 0x prefix) */
static uint32_t parse_hex(const char *str) __attribute__((unused));
static uint32_t parse_hex(const char *str) {
    uint32_t result = 0;
    /* Skip 0x prefix if present */
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
    }
    while (1) {
        char c = *str++;
        if (c >= '0' && c <= '9') {
            result = (result << 4) | (c - '0');
        } else if (c >= 'a' && c <= 'f') {
            result = (result << 4) | (c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            result = (result << 4) | (c - 'A' + 10);
        } else {
            break;
        }
    }
    return result;
}

/* Print hex number */
static void shell_print_hex(uint32_t num) {
    char hex_chars[] = "0123456789ABCDEF";
    char str[9];
    str[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        str[i] = hex_chars[num & 0xF];
        num >>= 4;
    }
    terminal_write("0x");
    terminal_write(str);
}

/* Print decimal number */
static void shell_print_dec(uint32_t num) {
    char buf[12];
    int i = 10;
    buf[11] = '\0';

    if (num == 0) {
        terminal_write("0");
        return;
    }

    while (num > 0 && i >= 0) {
        buf[i--] = '0' + (num % 10);
        num /= 10;
    }
    terminal_write(&buf[i + 1]);
}

/* Print memory size in human-readable format */
static void shell_print_size(uint32_t bytes) {
    if (bytes >= 1024 * 1024) {
        shell_print_dec(bytes / (1024 * 1024));
        terminal_write(" MB");
    } else if (bytes >= 1024) {
        shell_print_dec(bytes / 1024);
        terminal_write(" KB");
    } else {
        shell_print_dec(bytes);
        terminal_write(" bytes");
    }
}

/* Print a single hex byte */
static void shell_print_byte(uint8_t byte) {
    char hex_chars[] = "0123456789ABCDEF";
    terminal_putchar(hex_chars[(byte >> 4) & 0xF]);
    terminal_putchar(hex_chars[byte & 0xF]);
}

/* Hexdump memory at address */
static void shell_hexdump(uint32_t addr, uint32_t size) {
    uint8_t *ptr = (uint8_t *)addr;
    uint32_t i, j;

    for (i = 0; i < size; i += 16) {
        /* Print address */
        shell_print_hex(addr + i);
        terminal_write(": ");

        /* Print hex bytes */
        for (j = 0; j < 16 && (i + j) < size; j++) {
            shell_print_byte(ptr[i + j]);
            terminal_putchar(' ');
            if (j == 7) terminal_putchar(' ');
        }
        /* Pad if less than 16 bytes */
        for (; j < 16; j++) {
            terminal_write("   ");
            if (j == 7) terminal_putchar(' ');
        }

        terminal_write(" |");
        /* Print ASCII */
        for (j = 0; j < 16 && (i + j) < size; j++) {
            char c = ptr[i + j];
            if (c >= 32 && c < 127)
                terminal_putchar(c);
            else
                terminal_putchar('.');
        }
        terminal_write("|\n");
    }
}

/* Dump stack contents */
static void shell_dump_stack(void) {
    uint32_t esp, ebp;
    asm volatile("mov %%esp, %0" : "=r"(esp));
    asm volatile("mov %%ebp, %0" : "=r"(ebp));

    terminal_write("=== Stack Dump ===\n");
    terminal_write("ESP: ");
    shell_print_hex(esp);
    terminal_write("  EBP: ");
    shell_print_hex(ebp);
    terminal_write("\n\n");

    /* Dump 64 bytes of stack */
    terminal_write("Stack contents (64 bytes from ESP):\n");
    shell_hexdump(esp, 64);
}

/* Store last kmalloc allocation for free testing */
static void *last_alloc = NULL;
static size_t last_alloc_size = 0;

/*
 * shell_getchar: busy-polls the keyboard until a key press is detected.
 */
static char shell_getchar(void) {
    while (1) {
        if (inb(KEYBOARD_STATUS_PORT) & 0x01) {
            uint8_t scancode = inb(KEYBOARD_DATA_PORT);
            /* Only process key press events (ignore releases) */
            if (scancode < sizeof(scancode_to_ascii) && !(scancode & 0x80)) {
                char c = scancode_to_ascii[scancode];
                /* Wait until the key is released to avoid autorepeat issues */
                while (inb(KEYBOARD_STATUS_PORT) & 0x01)
                    inb(KEYBOARD_DATA_PORT);
                /* Only return valid characters, ignore unmapped keys */
                if (c != 0)
                    return c;
            }
        }
    }
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
            if (c == '\b') {
                if (pos > 0) {
                    pos--;
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
            terminal_write("  help          - Show this help message\n");
            terminal_write("  echo TEXT     - Print TEXT\n");
            terminal_write("  clear         - Clear the screen\n");
            terminal_write("  exit          - Exit the shell\n");
            terminal_write("  reboot        - Reboot the system\n");
            terminal_write("\nMemory commands:\n");
            terminal_write("  meminfo       - Show memory statistics\n");
            terminal_write("  alloc SIZE    - Allocate SIZE bytes with kmalloc\n");
            terminal_write("  free          - Free last allocation\n");
            terminal_write("  kheap         - Dump kernel heap state\n");
            terminal_write("  vmap          - Show virtual memory info\n");
            terminal_write("\nDebug commands:\n");
            terminal_write("  hexdump ADDR [SIZE] - Dump memory (default 64 bytes)\n");
            terminal_write("  peek ADDR     - Read 32-bit value at address\n");
            terminal_write("  poke ADDR VAL - Write 32-bit value to address\n");
            terminal_write("  stack         - Dump current stack\n");
            terminal_write("  pagedir       - Show page directory info\n");
            terminal_write("  panic         - Trigger test kernel panic\n");
        } else if (strncmp(buffer, "echo ", 5) == 0) {
            terminal_write(buffer + 5);
            terminal_putchar('\n');
        }
        else if (strcmp(buffer, "clear") == 0) {
            terminal_initialize();
        }
        else if (strcmp(buffer, "ls") == 0) {
            terminal_write("boot/\n");
            terminal_write("kernel/\n");
            terminal_write("tools/\n");
            terminal_write("README.md\n");
        }
        /* Memory info command */
        else if (strcmp(buffer, "meminfo") == 0) {
            pmm_stats_t pmm_stats;
            kmalloc_stats_t kmalloc_stats;
            vmalloc_stats_t vmalloc_stats;

            pmm_get_stats(&pmm_stats);
            kmalloc_get_stats(&kmalloc_stats);
            vmalloc_get_stats(&vmalloc_stats);

            terminal_write("\n=== Physical Memory ===\n");
            terminal_write("Total: ");
            shell_print_size(pmm_stats.total_memory);
            terminal_write("\nUsed:  ");
            shell_print_size(pmm_stats.used_frames * 4096);
            terminal_write(" (");
            shell_print_dec(pmm_stats.used_frames);
            terminal_write(" pages)\n");
            terminal_write("Free:  ");
            shell_print_size(pmm_stats.free_frames * 4096);
            terminal_write(" (");
            shell_print_dec(pmm_stats.free_frames);
            terminal_write(" pages)\n");

            terminal_write("\n=== Kernel Heap ===\n");
            terminal_write("Heap size: ");
            shell_print_size(kmalloc_stats.heap_size);
            terminal_write("\nUsed:      ");
            shell_print_size(kmalloc_stats.used_size);
            terminal_write("\nFree:      ");
            shell_print_size(kmalloc_stats.free_size);
            terminal_write("\nAllocs:    ");
            shell_print_dec(kmalloc_stats.num_allocations);
            terminal_write("\n");

            terminal_write("\n=== Virtual Memory ===\n");
            terminal_write("Allocated: ");
            shell_print_size(vmalloc_stats.total_virtual);
            terminal_write("\nRegions:   ");
            shell_print_dec(vmalloc_stats.num_regions);
            terminal_write("\n");
        }
        /* Allocate memory command */
        else if (strncmp(buffer, "alloc ", 6) == 0) {
            uint32_t size = parse_uint(buffer + 6);
            if (size == 0) {
                terminal_write("Usage: alloc SIZE (in bytes)\n");
            } else {
                void *ptr = kmalloc(size);
                if (ptr) {
                    terminal_write("Allocated ");
                    shell_print_dec(size);
                    terminal_write(" bytes at ");
                    shell_print_hex((uint32_t)ptr);
                    terminal_write("\n");
                    last_alloc = ptr;
                    last_alloc_size = size;
                } else {
                    terminal_write("Allocation failed!\n");
                }
            }
        }
        /* Free memory command */
        else if (strcmp(buffer, "free") == 0) {
            if (last_alloc == NULL) {
                terminal_write("No allocation to free\n");
            } else {
                terminal_write("Freeing ");
                shell_print_dec(last_alloc_size);
                terminal_write(" bytes at ");
                shell_print_hex((uint32_t)last_alloc);
                terminal_write("\n");
                kfree(last_alloc);
                last_alloc = NULL;
                last_alloc_size = 0;
            }
        }
        /* Kernel heap dump command */
        else if (strcmp(buffer, "kheap") == 0) {
            kmalloc_dump();
        }
        /* Virtual memory info command */
        else if (strcmp(buffer, "vmap") == 0) {
            vmalloc_dump();

            terminal_write("\n=== Page Directory Info ===\n");
            page_directory_t *pd = paging_get_directory();
            terminal_write("Page directory at: ");
            shell_print_hex((uint32_t)pd);
            terminal_write("\n");

            /* Count mapped page tables */
            int mapped_tables = 0;
            for (int i = 0; i < 1024; i++) {
                if (pd->entries[i] & PAGE_PRESENT) {
                    mapped_tables++;
                }
            }
            terminal_write("Mapped page tables: ");
            shell_print_dec(mapped_tables);
            terminal_write("/1024\n");
        }
        /* Test panic command */
        else if (strcmp(buffer, "panic") == 0) {
            terminal_write("Triggering test kernel panic...\n");
            panic("User-triggered test panic");
        }
        /* Hexdump command */
        else if (strncmp(buffer, "hexdump ", 8) == 0) {
            char *args = buffer + 8;
            uint32_t addr = parse_hex(args);
            uint32_t size = 64;  /* Default size */
            /* Find second argument (size) */
            while (*args && *args != ' ') args++;
            if (*args == ' ') {
                args++;
                uint32_t parsed_size = parse_uint(args);
                if (parsed_size > 0) size = parsed_size;
            }
            if (size > 512) size = 512;  /* Limit dump size */
            terminal_write("Memory dump at ");
            shell_print_hex(addr);
            terminal_write(" (");
            shell_print_dec(size);
            terminal_write(" bytes):\n");
            shell_hexdump(addr, size);
        }
        /* Peek command - read memory */
        else if (strncmp(buffer, "peek ", 5) == 0) {
            uint32_t addr = parse_hex(buffer + 5);
            uint32_t *ptr = (uint32_t *)addr;
            terminal_write("Value at ");
            shell_print_hex(addr);
            terminal_write(": ");
            shell_print_hex(*ptr);
            terminal_write(" (");
            shell_print_dec(*ptr);
            terminal_write(")\n");
        }
        /* Poke command - write memory */
        else if (strncmp(buffer, "poke ", 5) == 0) {
            char *args = buffer + 5;
            uint32_t addr = parse_hex(args);
            /* Find second argument (value) */
            while (*args && *args != ' ') args++;
            if (*args == ' ') {
                args++;
                uint32_t value = parse_hex(args);
                uint32_t *ptr = (uint32_t *)addr;
                *ptr = value;
                terminal_write("Wrote ");
                shell_print_hex(value);
                terminal_write(" to ");
                shell_print_hex(addr);
                terminal_write("\n");
            } else {
                terminal_write("Usage: poke ADDR VALUE\n");
            }
        }
        /* Stack dump command */
        else if (strcmp(buffer, "stack") == 0) {
            shell_dump_stack();
        }
        /* Page directory info command */
        else if (strcmp(buffer, "pagedir") == 0) {
            page_directory_t *pd = paging_get_directory();
            terminal_write("=== Page Directory ===\n");
            terminal_write("Address: ");
            shell_print_hex((uint32_t)pd);
            terminal_write("\n\nMapped page tables:\n");
            int count = 0;
            for (int i = 0; i < 1024; i++) {
                if (pd->entries[i] & PAGE_PRESENT) {
                    terminal_write("  [");
                    shell_print_dec(i);
                    terminal_write("] -> ");
                    shell_print_hex(pd->entries[i] & 0xFFFFF000);
                    terminal_write(" (");
                    if (pd->entries[i] & PAGE_WRITE) terminal_write("RW");
                    else terminal_write("RO");
                    if (pd->entries[i] & PAGE_USER) terminal_write(",U");
                    else terminal_write(",K");
                    terminal_write(") VA: ");
                    shell_print_hex(i * 4 * 1024 * 1024);
                    terminal_write("-");
                    shell_print_hex((i + 1) * 4 * 1024 * 1024 - 1);
                    terminal_write("\n");
                    count++;
                    if (count >= 10) {
                        terminal_write("  ... (");
                        int total = 0;
                        for (int j = 0; j < 1024; j++)
                            if (pd->entries[j] & PAGE_PRESENT) total++;
                        shell_print_dec(total);
                        terminal_write(" total)\n");
                        break;
                    }
                }
            }
        }
        /* Reboot command */
        else if (strcmp(buffer, "reboot") == 0) {
            terminal_write("Rebooting...\n");
            outb(0x64, 0xFE);
        }
        else if (strcmp(buffer, "exit") == 0) {
            terminal_write("Exiting shell...\n");
            break;
        } else if (buffer[0] != '\0') {
            terminal_write("Command not found. Type 'help' for commands.\n");
        }
    }
}
