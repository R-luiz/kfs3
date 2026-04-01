#include "keyboard.h"
#include "idt.h"
#include "signals.h"

#define KEYBOARD_BUFFER_SIZE  256

static const char scancode_to_ascii[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

static const char scancode_to_ascii_shift[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};

static volatile char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint32_t keyboard_head = 0;
static volatile uint32_t keyboard_tail = 0;
static volatile int shift_held = 0;
static volatile int extended_prefix = 0;

static void keyboard_queue_char(char c)
{
    uint32_t next_tail = (keyboard_tail + 1) % KEYBOARD_BUFFER_SIZE;

    if (next_tail == keyboard_head) {
        return;
    }

    keyboard_buffer[keyboard_tail] = c;
    keyboard_tail = next_tail;
}

static void keyboard_interrupt(cpu_registers_t *registers)
{
    (void)registers;
    keyboard_handler();
}

void keyboard_handler(void)
{
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    /* Track shift state */
    if (scancode == 0x2A || scancode == 0x36) { shift_held = 1; return; }
    if (scancode == 0xAA || scancode == 0xB6) { shift_held = 0; return; }

    /* Extended scancode prefix */
    if (scancode == 0xE0) { extended_prefix = 1; return; }

    if (extended_prefix) {
        extended_prefix = 0;
        if (scancode & 0x80) return; /* release of extended key */
        char key = 0;
        switch (scancode) {
        case 0x48: key = KEY_ARROW_UP;   break;
        case 0x50: key = KEY_ARROW_DOWN; break;
        case 0x49: key = KEY_PAGE_UP;    break;
        case 0x51: key = KEY_PAGE_DOWN;  break;
        }
        if (key) {
            keyboard_queue_char(key);
            schedule_signal(KERNEL_SIGNAL_KEYBOARD, (uint32_t)(uint8_t)key, NULL);
        }
        return;
    }

    /* Ignore key releases and out-of-range scancodes */
    if ((scancode & 0x80) != 0 || scancode >= sizeof(scancode_to_ascii))
        return;

    {
        const char *table = shift_held ? scancode_to_ascii_shift : scancode_to_ascii;
        char c = table[scancode];
        if (c != 0) {
            keyboard_queue_char(c);
            schedule_signal(KERNEL_SIGNAL_KEYBOARD, (uint32_t)(uint8_t)c, NULL);
        }
    }
}

int keyboard_shift_held(void)
{
    return shift_held;
}

void init_keyboard(void)
{
    keyboard_head = 0;
    keyboard_tail = 0;

    while (inb(KEYBOARD_STATUS_PORT) & 0x01) {
        inb(KEYBOARD_DATA_PORT);
    }

    idt_register_handler(IRQ1_VECTOR, keyboard_interrupt);
    pic_clear_mask(1);
    pic_clear_mask(2);
}

int keyboard_has_char(void)
{
    return keyboard_head != keyboard_tail;
}

int keyboard_try_read_char(char *out_char)
{
    uint32_t flags;

    if (out_char == NULL) {
        return 0;
    }

    flags = interrupts_save();
    if (keyboard_head == keyboard_tail) {
        interrupts_restore(flags);
        return 0;
    }

    *out_char = keyboard_buffer[keyboard_head];
    keyboard_head = (keyboard_head + 1) % KEYBOARD_BUFFER_SIZE;
    interrupts_restore(flags);
    return 1;
}

char keyboard_getchar(void)
{
    char c;

    while (!keyboard_try_read_char(&c)) {
        __asm__ volatile("hlt");
    }
    return c;
}

ssize_t keyboard_get_line(char *buffer, size_t buffer_size)
{
    size_t position = 0;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }

    while (position + 1 < buffer_size) {
        char c = keyboard_getchar();

        if (c == '\b') {
            if (position > 0) {
                position--;
            }
            continue;
        }

        buffer[position++] = c;
        if (c == '\n') {
            break;
        }
    }

    buffer[position] = '\0';
    return (ssize_t)position;
}

ssize_t get_line(char *buffer, size_t buffer_size)
{
    return keyboard_get_line(buffer, buffer_size);
}