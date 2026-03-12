#include "serial.h"
#include "io.h"

#define SERIAL_COM1                     0x3F8
#define SERIAL_DATA_PORT                (SERIAL_COM1 + 0)
#define SERIAL_INTERRUPT_ENABLE_PORT    (SERIAL_COM1 + 1)
#define SERIAL_FIFO_CONTROL_PORT        (SERIAL_COM1 + 2)
#define SERIAL_LINE_CONTROL_PORT        (SERIAL_COM1 + 3)
#define SERIAL_MODEM_CONTROL_PORT       (SERIAL_COM1 + 4)
#define SERIAL_LINE_STATUS_PORT         (SERIAL_COM1 + 5)

static int serial_enabled = 0;

static int serial_transmit_ready(void)
{
    return (inb(SERIAL_LINE_STATUS_PORT) & 0x20) != 0;
}

void serial_init(void)
{
    outb(SERIAL_INTERRUPT_ENABLE_PORT, 0x00);
    outb(SERIAL_LINE_CONTROL_PORT, 0x80);
    outb(SERIAL_DATA_PORT, 0x03);
    outb(SERIAL_INTERRUPT_ENABLE_PORT, 0x00);
    outb(SERIAL_LINE_CONTROL_PORT, 0x03);
    outb(SERIAL_FIFO_CONTROL_PORT, 0xC7);
    outb(SERIAL_MODEM_CONTROL_PORT, 0x0F);
    serial_enabled = 1;
}

int serial_is_enabled(void)
{
    return serial_enabled;
}

int serial_has_data(void)
{
    return serial_enabled && ((inb(SERIAL_LINE_STATUS_PORT) & 0x01) != 0);
}

char serial_read_char(void)
{
    while (!serial_has_data()) {
    }
    return (char)inb(SERIAL_DATA_PORT);
}

void serial_write_char(char c)
{
    if (!serial_enabled) {
        return;
    }

    if (c == '\n') {
        while (!serial_transmit_ready()) {
        }
        outb(SERIAL_DATA_PORT, '\r');
    } else if (c == '\b') {
        while (!serial_transmit_ready()) {
        }
        outb(SERIAL_DATA_PORT, '\b');
        while (!serial_transmit_ready()) {
        }
        outb(SERIAL_DATA_PORT, ' ');
        while (!serial_transmit_ready()) {
        }
        outb(SERIAL_DATA_PORT, '\b');
        return;
    }

    while (!serial_transmit_ready()) {
    }
    outb(SERIAL_DATA_PORT, (uint8_t)c);
}

void serial_write(const char *str)
{
    if (!serial_enabled || str == NULL) {
        return;
    }

    while (*str) {
        serial_write_char(*str++);
    }
}