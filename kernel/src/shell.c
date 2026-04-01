#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "serial.h"
#include "types.h"
#include "idt.h"
#include "pmm.h"
#include "kmalloc.h"
#include "vmalloc.h"
#include "paging.h"
#include "signals.h"
#include "syscall.h"
#include "process.h"
#include "timer.h"
#include "panic.h"

#define SHELL_BUFFER_SIZE 256

/* External terminal routines */
extern void terminal_putchar(char c);
extern void terminal_write(const char* str);
extern void terminal_initialize(void);
extern void terminal_scroll_up(size_t lines);
extern void terminal_scroll_down(size_t lines);

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
static int shell_signal_handler_installed = 0;
static int shell_last_signal = 0;
static uint32_t shell_last_signal_value = 0;
static uint32_t shell_signal_count = 0;
static uint32_t shell_last_demo_pid = 0;

static char *skip_spaces(char *text)
{
    while (*text == ' ') {
        text++;
    }
    return text;
}

static void shell_print_process_summary(process_t *process)
{
    uint32_t parent_pid = process->parent != NULL ? process->parent->pid : 0;

    terminal_write("PID=");
    shell_print_dec(process->pid);
    terminal_write(" STATE=");
    terminal_write(process_status_name(process->status));
    terminal_write(" OWNER=");
    shell_print_dec(process->owner);
    terminal_write(" PARENT=");
    shell_print_dec(parent_pid);
    terminal_write(" RUNS=");
    shell_print_dec(process->run_count);
    terminal_write(" TICKS=");
    shell_print_dec(process->cpu_ticks);
    terminal_write(" SIGQ=");
    shell_print_dec(process_signal_pending_count(process));
    terminal_write(" SOCKQ=");
    shell_print_dec(process_socket_pending_count(process));
    terminal_write(" CR3=");
    shell_print_hex((uint32_t)process->memory.process_page_directory);
    terminal_write(" PAGE=");
    shell_print_hex(process->memory.process_page);
    terminal_write(" HEAP=");
    shell_print_hex(process->memory.heap_start);
    terminal_write(" STACK=");
    shell_print_hex(process->memory.stack_top);
    terminal_write(" ESP=");
    shell_print_hex(process->context.esp);
    terminal_write(" SWITCH=");
    shell_print_dec(process->switch_count);
    terminal_write(" LASTSIG=");
    shell_print_dec(process->last_signal);
    terminal_write(" SIGVAL=");
    shell_print_hex(process->last_signal_value);
    terminal_write(" LASTVAL=");
    shell_print_hex(process->runtime_value);
    if (process->status == PROCESS_STATUS_ZOMBIE) {
        terminal_write(" EXIT=");
        shell_print_dec((uint32_t)process->exit_code);
    }
    terminal_write("\n");
}

static void shell_signal_handler(int signal, uint32_t value, void *context)
{
    (void)context;

    shell_last_signal = signal;
    shell_last_signal_value = value;
    shell_signal_count++;

    terminal_write("[signal] handled signal ");
    shell_print_dec((uint32_t)signal);
    terminal_write(" value=");
    shell_print_hex(value);
    terminal_write("\n");
}

/* Exception test handlers for div0/invop commands */
static volatile int exception_test_caught = 0;
static volatile uint32_t exception_test_vector = 0;
static volatile uint32_t exception_test_eip = 0;

static void div0_test_handler(cpu_registers_t *regs)
{
    exception_test_caught = 1;
    exception_test_vector = regs->int_no;
    exception_test_eip = regs->eip;
    regs->eip += 2; /* skip div %%ecx (F7 F1 = 2 bytes) */
}

static void invop_test_handler(cpu_registers_t *regs)
{
    exception_test_caught = 1;
    exception_test_vector = regs->int_no;
    exception_test_eip = regs->eip;
    regs->eip += 2; /* skip ud2 (0F 0B = 2 bytes) */
}

/*
 * shell_getchar: reads shell input from serial or the keyboard IRQ buffer.
 * Scroll keys are handled internally and never returned to the caller.
 */
static char shell_getchar(void) {
    while (1) {
        char keyboard_char;

        signal_dispatch_pending();
        scheduler_run_pending(1);

        if (serial_has_data()) {
            char c = serial_read_char();

            if (c == '\r' || c == '\n') {
                return '\n';
            }
            if (c == '\b' || c == 0x7F) {
                return '\b';
            }
            if (c == '\t' || (c >= 32 && c < 127)) {
                return c;
            }
        }

        if (keyboard_try_read_char(&keyboard_char)) {
            if (keyboard_char == KEY_PAGE_UP) {
                terminal_scroll_up(24);
                continue;
            }
            if (keyboard_char == KEY_PAGE_DOWN) {
                terminal_scroll_down(24);
                continue;
            }
            if (keyboard_char == KEY_ARROW_UP && keyboard_shift_held()) {
                terminal_scroll_up(1);
                continue;
            }
            if (keyboard_char == KEY_ARROW_DOWN && keyboard_shift_held()) {
                terminal_scroll_down(1);
                continue;
            }
            return keyboard_char;
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
            terminal_write("\nMemory tests:\n");
            terminal_write("  pagetest      - Test page map/write/unmap round-trip\n");
            terminal_write("  pageperm      - Test page permission flags\n");
            terminal_write("  pmmtest       - Test PMM frame alloc/free accounting\n");
            terminal_write("  vmtest        - Test vmalloc alloc/write/free\n");
            terminal_write("  heaptest      - Test heap multi-alloc/free with reuse\n");
            terminal_write("\nDebug commands:\n");
            terminal_write("  hexdump ADDR [SIZE] - Dump memory (default 64 bytes)\n");
            terminal_write("  peek ADDR     - Read 32-bit value at address\n");
            terminal_write("  poke ADDR VAL - Write 32-bit value to address\n");
            terminal_write("  stack         - Dump current stack\n");
            terminal_write("  pagedir       - Show page directory info\n");
            terminal_write("  idt           - Show IDT and interrupt state\n");
            terminal_write("  signals       - Show signal queue state\n");
            terminal_write("  sigtest       - Schedule and handle a test signal\n");
            terminal_write("  syscall       - Trigger int 0x80 test syscall\n");
            terminal_write("  int3          - Trigger a breakpoint exception\n");
            terminal_write("  panic         - Trigger test kernel panic\n");
            terminal_write("  div0          - Test division by zero exception\n");
            terminal_write("  invop         - Test invalid opcode exception\n");
            terminal_write("  segreg        - Show segment registers\n");
            terminal_write("\nProcess commands:\n");
            terminal_write("  procs         - Show the process table\n");
            terminal_write("  execdemo      - Spawn demo counter processes\n");
            terminal_write("  sched TICKS   - Force scheduler ticks\n");
            terminal_write("  forkdemo PID  - Fork a process\n");
            terminal_write("  waitproc PID  - Collect a zombie child\n");
            terminal_write("  killproc PID [SIG] - Queue a signal or kill a process\n");
            terminal_write("  sigproc PID SIG [VAL] - Queue a process signal\n");
            terminal_write("  getuid PID    - Show the owner of a process\n");
            terminal_write("  mmapproc PID SIZE - Reserve memory in a process heap\n");
            terminal_write("  sockdemo      - Spawn a socket IPC demo pair\n");
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
        else if (strcmp(buffer, "idt") == 0) {
            uint32_t base;
            uint16_t limit;

            idt_get_descriptor(&base, &limit);
            terminal_write("=== IDT ===\n");
            terminal_write("Base: ");
            shell_print_hex(base);
            terminal_write("\nLimit: ");
            shell_print_dec(limit);
            terminal_write(" bytes\nInterrupts: ");
            terminal_write(idt_interrupts_enabled() ? "enabled" : "disabled");
            terminal_write("\nIRQ1 vector: 0x00000021\nSystem call vector: 0x00000080\n");
        }
        else if (strcmp(buffer, "signals") == 0) {
            terminal_write("=== Signals ===\n");
            terminal_write("Pending: ");
            shell_print_dec(signal_pending_count());
            terminal_write("\nHandled: ");
            shell_print_dec(shell_signal_count);
            terminal_write("\nLast signal: ");
            shell_print_dec((uint32_t)shell_last_signal);
            terminal_write("\nLast value: ");
            shell_print_hex(shell_last_signal_value);
            terminal_write("\n");
        }
        else if (strcmp(buffer, "sigtest") == 0) {
            if (!shell_signal_handler_installed) {
                register_signal_handler(KERNEL_SIGNAL_TEST, shell_signal_handler);
                shell_signal_handler_installed = 1;
            }

            terminal_write("Scheduling test signal...\n");
            if (schedule_signal(KERNEL_SIGNAL_TEST, 0xC0DE1234, NULL) == 0) {
                signal_dispatch_pending();
                terminal_write("Signal delivery complete.\n");
            } else {
                terminal_write("Signal queue is full.\n");
            }
        }
        else if (strcmp(buffer, "syscall") == 0) {
            uint32_t result;

            asm volatile ("int $0x80" : "=a"(result) : "a"(SYSCALL_GET_MAGIC), "b"(0), "c"(0), "d"(0), "S"(0) : "memory");
            terminal_write("syscall returned ");
            shell_print_hex(result);
            terminal_write("\n");
        }
        else if (strcmp(buffer, "procs") == 0) {
            process_t *process = process_first();

            terminal_write("=== Process Table ===\n");
            terminal_write("Total: ");
            shell_print_dec(process_count());
            terminal_write("  CPU ticks: ");
            shell_print_dec(scheduler_tick_count());
            terminal_write("  PIT ticks: ");
            shell_print_dec(timer_get_ticks());
            terminal_write("  Context switches: ");
            shell_print_dec(process_context_switch_count());
            terminal_write("\n");

            while (process != NULL) {
                shell_print_process_summary(process);
                process = process->next;
            }
        }
        else if (strcmp(buffer, "execdemo") == 0) {
            process_t *first = process_spawn_demo_counter(1000, 0, 3);
            process_t *second = process_spawn_demo_counter(1001, 10, 13);

            if (first == NULL || second == NULL) {
                terminal_write("Failed to create demo processes\n");
            } else {
                shell_last_demo_pid = second->pid;
                terminal_write("Created demo processes: ");
                shell_print_dec(first->pid);
                terminal_write(", ");
                shell_print_dec(second->pid);
                terminal_write("\n");
            }
        }
        else if (strncmp(buffer, "sched ", 6) == 0) {
            uint32_t ticks = parse_uint(buffer + 6);

            if (ticks == 0) {
                terminal_write("Usage: sched TICKS\n");
            } else {
                scheduler_force_ticks(ticks);
                terminal_write("Scheduler ran ");
                shell_print_dec(ticks);
                terminal_write(" ticks (total ");
                shell_print_dec(scheduler_tick_count());
                terminal_write(")\n");
            }
        }
        else if (strncmp(buffer, "forkdemo", 8) == 0) {
            char *args = skip_spaces(buffer + 8);
            uint32_t pid = *args != '\0' ? parse_uint(args) : shell_last_demo_pid;
            process_t *source = process_get_by_pid(pid);
            process_t *child;

            if (source == NULL) {
                terminal_write("Usage: forkdemo PID\n");
            } else {
                child = fork_process(source);
                if (child == NULL) {
                    terminal_write("Fork failed\n");
                } else {
                    terminal_write("Forked PID ");
                    shell_print_dec(pid);
                    terminal_write(" -> child PID ");
                    shell_print_dec(child->pid);
                    terminal_write("\n");
                }
            }
        }
        else if (strncmp(buffer, "waitproc ", 9) == 0) {
            uint32_t pid = parse_uint(buffer + 9);
            int exit_code = 0;
            int result = process_wait(process_root(), pid, &exit_code);

            if (result < 0) {
                terminal_write("waitproc: invalid child pid\n");
            } else if (result == 0) {
                terminal_write("waitproc: process still running\n");
            } else {
                terminal_write("wait collected PID ");
                shell_print_dec(pid);
                terminal_write(" exit=");
                shell_print_dec((uint32_t)exit_code);
                terminal_write("\n");
            }
        }
        else if (strncmp(buffer, "killproc ", 9) == 0) {
            char *args = buffer + 9;
            uint32_t pid = parse_uint(args);
            int signal = PROCESS_SIGNAL_TERM;

            while (*args && *args != ' ') args++;
            if (*args == ' ') {
                args = skip_spaces(args);
                signal = (int)parse_uint(args);
            }

            if (kill_process(process_root(), pid, signal) == 0) {
                terminal_write("Queued signal ");
                shell_print_dec((uint32_t)signal);
                terminal_write(" for PID ");
                shell_print_dec(pid);
                terminal_write("\n");
            } else {
                terminal_write("killproc failed\n");
            }
        }
        else if (strncmp(buffer, "sigproc ", 8) == 0) {
            char *args = buffer + 8;
            uint32_t pid = parse_uint(args);
            int signal;
            uint32_t value = 0;

            while (*args && *args != ' ') args++;
            if (*args != ' ') {
                terminal_write("Usage: sigproc PID SIG [VAL]\n");
            } else {
                args = skip_spaces(args);
                signal = (int)parse_uint(args);
                while (*args && *args != ' ') args++;
                if (*args == ' ') {
                    args = skip_spaces(args);
                    value = parse_hex(args);
                }

                if (send_signal_to_process(process_root(), pid, signal, value) == 0) {
                    terminal_write("Queued process signal ");
                    shell_print_dec((uint32_t)signal);
                    terminal_write(" for PID ");
                    shell_print_dec(pid);
                    terminal_write(" value=");
                    shell_print_hex(value);
                    terminal_write("\n");
                } else {
                    terminal_write("sigproc failed\n");
                }
            }
        }
        else if (strncmp(buffer, "getuid ", 7) == 0) {
            uint32_t pid = parse_uint(buffer + 7);
            process_t *process = process_get_by_pid(pid);

            if (process == NULL) {
                terminal_write("getuid: invalid pid\n");
            } else {
                terminal_write("PID ");
                shell_print_dec(pid);
                terminal_write(" owner=");
                shell_print_dec(process_getuid(process));
                terminal_write("\n");
            }
        }
        else if (strncmp(buffer, "mmapproc ", 9) == 0) {
            char *args = buffer + 9;
            uint32_t pid = parse_uint(args);
            process_t *process;
            void *mapped;

            while (*args && *args != ' ') args++;
            if (*args != ' ') {
                terminal_write("Usage: mmapproc PID SIZE\n");
            } else {
                args = skip_spaces(args);
                process = process_get_by_pid(pid);
                if (process == NULL) {
                    terminal_write("mmapproc: invalid pid\n");
                } else {
                    mapped = process_mmap(process, parse_uint(args));
                    if (mapped == NULL) {
                        terminal_write("mmapproc failed\n");
                    } else {
                        terminal_write("mmapproc mapped ");
                        shell_print_hex((uint32_t)mapped);
                        terminal_write(" for PID ");
                        shell_print_dec(pid);
                        terminal_write("\n");
                    }
                }
            }
        }
        else if (strcmp(buffer, "sockdemo") == 0) {
            uint32_t sender_pid = 0;
            uint32_t receiver_pid = 0;
            process_t *receiver;

            if (process_spawn_demo_socket_pair(2000, 0xABCD1234, &sender_pid, &receiver_pid) != 0) {
                terminal_write("Socket demo setup failed\n");
            } else {
                scheduler_force_ticks(4);
                receiver = process_get_by_pid(receiver_pid);
                terminal_write("Socket demo sender=");
                shell_print_dec(sender_pid);
                terminal_write(" receiver=");
                shell_print_dec(receiver_pid);
                terminal_write(" value=");
                if (receiver != NULL) {
                    shell_print_hex(receiver->last_socket_value);
                } else {
                    terminal_write("0x00000000");
                }
                terminal_write("\n");
            }
        }
        /* --- Memory test commands --- */
        else if (strcmp(buffer, "pagetest") == 0) {
            uint32_t test_vaddr = 0x40000000;
            uint32_t frame = pmm_alloc_frame();
            if (frame == 0) {
                terminal_write("pagetest: FAIL (no free frames)\n");
            } else {
                uint32_t readback;
                terminal_write("pagetest: alloc frame=");
                shell_print_hex(frame);
                terminal_write("\n");

                paging_map_page(test_vaddr, frame, PAGE_KERNEL);
                terminal_write("pagetest: mapped VA=");
                shell_print_hex(test_vaddr);
                terminal_write(" -> PA=");
                shell_print_hex(frame);
                terminal_write("\n");

                volatile uint32_t *ptr = (volatile uint32_t *)test_vaddr;
                *ptr = 0xCAFE4242;
                terminal_write("pagetest: write 0xCAFE4242 OK\n");

                readback = *ptr;
                terminal_write("pagetest: read ");
                shell_print_hex(readback);
                terminal_write(readback == 0xCAFE4242 ? " OK\npagetest: match OK\n" : " FAIL\n");

                paging_unmap_page(test_vaddr);
                terminal_write("pagetest: unmapped VA=");
                shell_print_hex(test_vaddr);
                terminal_write("\n");

                pmm_free_frame(frame);
                terminal_write("pagetest: freed frame\n");

                terminal_write(readback == 0xCAFE4242 ? "pagetest: PASS\n" : "pagetest: FAIL\n");
            }
        }
        else if (strcmp(buffer, "pageperm") == 0) {
            uint32_t test_vaddr = 0x40000000;
            uint32_t frame = pmm_alloc_frame();
            if (frame == 0) {
                terminal_write("pageperm: FAIL (no free frames)\n");
            } else {
                page_directory_t *pd = paging_get_directory();
                uint32_t dir_idx = PAGE_DIR_INDEX(test_vaddr);
                uint32_t tbl_idx = PAGE_TABLE_INDEX(test_vaddr);
                page_table_t *table;
                uint32_t pte;
                int p, w, u, pass = 1;

                /* Test PAGE_KERNEL (Present, Write, not User) */
                paging_map_page(test_vaddr, frame, PAGE_KERNEL);
                table = (page_table_t *)PAGE_FRAME(pd->entries[dir_idx]);
                pte = table->entries[tbl_idx];
                p = (pte & PAGE_PRESENT) ? 1 : 0;
                w = (pte & PAGE_WRITE) ? 1 : 0;
                u = (pte & PAGE_USER) ? 1 : 0;
                terminal_write("pageperm: KERNEL -> P=");
                shell_print_dec(p);
                terminal_write(" W=");
                shell_print_dec(w);
                terminal_write(" U=");
                shell_print_dec(u);
                if (p == 1 && w == 1 && u == 0) {
                    terminal_write(" OK\n");
                } else { terminal_write(" FAIL\n"); pass = 0; }

                /* Test PAGE_USER_RO */
                paging_map_page(test_vaddr, frame, PAGE_USER_RO);
                table = (page_table_t *)PAGE_FRAME(pd->entries[dir_idx]);
                pte = table->entries[tbl_idx];
                p = (pte & PAGE_PRESENT) ? 1 : 0;
                w = (pte & PAGE_WRITE) ? 1 : 0;
                u = (pte & PAGE_USER) ? 1 : 0;
                terminal_write("pageperm: USER_RO -> P=");
                shell_print_dec(p);
                terminal_write(" W=");
                shell_print_dec(w);
                terminal_write(" U=");
                shell_print_dec(u);
                if (p == 1 && w == 0 && u == 1) {
                    terminal_write(" OK\n");
                } else { terminal_write(" FAIL\n"); pass = 0; }

                /* Test PAGE_USER_RW */
                paging_map_page(test_vaddr, frame, PAGE_USER_RW);
                table = (page_table_t *)PAGE_FRAME(pd->entries[dir_idx]);
                pte = table->entries[tbl_idx];
                p = (pte & PAGE_PRESENT) ? 1 : 0;
                w = (pte & PAGE_WRITE) ? 1 : 0;
                u = (pte & PAGE_USER) ? 1 : 0;
                terminal_write("pageperm: USER_RW -> P=");
                shell_print_dec(p);
                terminal_write(" W=");
                shell_print_dec(w);
                terminal_write(" U=");
                shell_print_dec(u);
                if (p == 1 && w == 1 && u == 1) {
                    terminal_write(" OK\n");
                } else { terminal_write(" FAIL\n"); pass = 0; }

                paging_unmap_page(test_vaddr);
                pmm_free_frame(frame);
                terminal_write(pass ? "pageperm: PASS\n" : "pageperm: FAIL\n");
            }
        }
        else if (strcmp(buffer, "pmmtest") == 0) {
            pmm_stats_t stats;
            int pass = 1;
            uint32_t initial_free, f1, f2, f3, contig;
            int32_t delta;

            pmm_get_stats(&stats);
            initial_free = stats.free_frames;
            terminal_write("pmmtest: initial free=");
            shell_print_dec(initial_free);
            terminal_write("\n");

            f1 = pmm_alloc_frame();
            f2 = pmm_alloc_frame();
            f3 = pmm_alloc_frame();
            if (f1 == 0 || f2 == 0 || f3 == 0) {
                terminal_write("pmmtest: FAIL (alloc returned 0)\n");
            } else {
                pmm_get_stats(&stats);
                delta = (int32_t)stats.free_frames - (int32_t)initial_free;
                terminal_write("pmmtest: alloc 3 frames OK (free=");
                shell_print_dec(stats.free_frames);
                terminal_write(")\npmmtest: delta=");
                if (delta < 0) { terminal_write("-"); shell_print_dec((uint32_t)(-delta)); }
                else { shell_print_dec((uint32_t)delta); }
                if (delta == -3) { terminal_write(" OK\n"); }
                else { terminal_write(" FAIL\n"); pass = 0; }

                pmm_free_frame(f1);
                pmm_free_frame(f2);
                pmm_free_frame(f3);
                pmm_get_stats(&stats);
                terminal_write("pmmtest: freed 3 frames (free=");
                shell_print_dec(stats.free_frames);
                terminal_write(")\n");
                if (stats.free_frames == initial_free) {
                    terminal_write("pmmtest: restored OK\n");
                } else { terminal_write("pmmtest: restored FAIL\n"); pass = 0; }

                contig = pmm_alloc_frames(4);
                if (contig == 0) {
                    terminal_write("pmmtest: contiguous alloc FAIL\n");
                    pass = 0;
                } else {
                    terminal_write("pmmtest: contiguous alloc 4 at ");
                    shell_print_hex(contig);
                    terminal_write(" OK\n");
                    pmm_free_frames(contig, 4);
                    pmm_get_stats(&stats);
                    terminal_write("pmmtest: contiguous free OK (free=");
                    shell_print_dec(stats.free_frames);
                    terminal_write(")\n");
                    if (stats.free_frames != initial_free) pass = 0;
                }
                terminal_write(pass ? "pmmtest: PASS\n" : "pmmtest: FAIL\n");
            }
        }
        else if (strcmp(buffer, "vmtest") == 0) {
            int pass = 1;
            void *region = vmalloc(8192);
            if (region == NULL) {
                terminal_write("vmtest: FAIL (vmalloc returned NULL)\n");
            } else {
                volatile uint32_t *p0 = (volatile uint32_t *)region;
                volatile uint32_t *p1 = (volatile uint32_t *)((uint8_t *)region + 4096);
                terminal_write("vmtest: vmalloc 8192 at ");
                shell_print_hex((uint32_t)region);
                terminal_write("\n");

                *p0 = 0xDEAD0001;
                terminal_write("vmtest: write page0 OK\n");
                *p1 = 0xDEAD0002;
                terminal_write("vmtest: write page1 OK\n");

                if (*p0 == 0xDEAD0001) { terminal_write("vmtest: read page0 OK\n"); }
                else { terminal_write("vmtest: read page0 FAIL\n"); pass = 0; }
                if (*p1 == 0xDEAD0002) { terminal_write("vmtest: read page1 OK\n"); }
                else { terminal_write("vmtest: read page1 FAIL\n"); pass = 0; }

                vfree(region);
                terminal_write("vmtest: vfree OK\n");
                terminal_write(pass ? "vmtest: PASS\n" : "vmtest: FAIL\n");
            }
        }
        else if (strcmp(buffer, "heaptest") == 0) {
            int pass = 1;
            kmalloc_stats_t stats;
            void *a, *b, *c, *d;
            uint32_t b_addr, d_addr;
            int reuse;

            a = kmalloc(64);
            b = kmalloc(128);
            c = kmalloc(64);
            terminal_write("heaptest: alloc A=64 at ");
            shell_print_hex((uint32_t)a);
            terminal_write("\nheaptest: alloc B=128 at ");
            shell_print_hex((uint32_t)b);
            terminal_write("\nheaptest: alloc C=64 at ");
            shell_print_hex((uint32_t)c);
            terminal_write("\n");

            b_addr = (uint32_t)b;
            kfree(b);
            terminal_write("heaptest: free B OK\n");

            d = kmalloc(64);
            d_addr = (uint32_t)d;
            reuse = (d_addr >= b_addr && d_addr < b_addr + 128);
            terminal_write("heaptest: alloc D=64 at ");
            shell_print_hex(d_addr);
            terminal_write(" (reuse=");
            terminal_write(reuse ? "YES" : "NO");
            terminal_write(")\n");

            kfree(a);
            terminal_write("heaptest: free A OK\n");
            kfree(c);
            terminal_write("heaptest: free C OK\n");
            kfree(d);
            terminal_write("heaptest: free D OK\n");

            kmalloc_get_stats(&stats);
            terminal_write("heaptest: allocs=");
            shell_print_dec(stats.num_allocations);
            if (stats.num_allocations == 0) { terminal_write(" OK\n"); }
            else { terminal_write(" FAIL\n"); pass = 0; }
            terminal_write(pass ? "heaptest: PASS\n" : "heaptest: FAIL\n");
        }
        /* --- Exception/interrupt test commands --- */
        else if (strcmp(buffer, "div0") == 0) {
            exception_test_caught = 0;
            terminal_write("div0: installing handler\n");
            idt_register_handler(0, div0_test_handler);
            terminal_write("div0: triggering division by zero\n");
            asm volatile(
                "xor %%edx, %%edx\n\t"
                "mov $1, %%eax\n\t"
                "xor %%ecx, %%ecx\n\t"
                "div %%ecx\n\t"
                : : : "eax", "ecx", "edx"
            );
            idt_unregister_handler(0);
            if (exception_test_caught) {
                terminal_write("div0: caught exception ");
                shell_print_dec(exception_test_vector);
                terminal_write(" (Division by zero) at EIP=");
                shell_print_hex(exception_test_eip);
                terminal_write("\ndiv0: PASS\n");
            } else {
                terminal_write("div0: FAIL (exception not caught)\n");
            }
        }
        else if (strcmp(buffer, "invop") == 0) {
            exception_test_caught = 0;
            terminal_write("invop: installing handler\n");
            idt_register_handler(6, invop_test_handler);
            terminal_write("invop: triggering invalid opcode\n");
            asm volatile(".byte 0x0F, 0x0B"); /* ud2 */
            idt_unregister_handler(6);
            if (exception_test_caught) {
                terminal_write("invop: caught exception ");
                shell_print_dec(exception_test_vector);
                terminal_write(" (Invalid opcode) at EIP=");
                shell_print_hex(exception_test_eip);
                terminal_write("\ninvop: PASS\n");
            } else {
                terminal_write("invop: FAIL (exception not caught)\n");
            }
        }
        else if (strcmp(buffer, "segreg") == 0) {
            uint16_t cs, ds, es, fs, gs, ss;
            asm volatile("mov %%cs, %0" : "=r"(cs));
            asm volatile("mov %%ds, %0" : "=r"(ds));
            asm volatile("mov %%es, %0" : "=r"(es));
            asm volatile("mov %%fs, %0" : "=r"(fs));
            asm volatile("mov %%gs, %0" : "=r"(gs));
            asm volatile("mov %%ss, %0" : "=r"(ss));
            terminal_write("CS=");
            shell_print_hex((uint32_t)cs);
            terminal_write(" DS=");
            shell_print_hex((uint32_t)ds);
            terminal_write(" ES=");
            shell_print_hex((uint32_t)es);
            terminal_write(" FS=");
            shell_print_hex((uint32_t)fs);
            terminal_write(" GS=");
            shell_print_hex((uint32_t)gs);
            terminal_write(" SS=");
            shell_print_hex((uint32_t)ss);
            terminal_write("\n");
        }
        else if (strcmp(buffer, "int3") == 0) {
            terminal_write("Triggering breakpoint exception...\n");
            asm volatile ("int3");
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

        scheduler_run_pending(2);
    }
}
