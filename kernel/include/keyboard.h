#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"
#include "io.h"

// Keyboard I/O ports
#define KEYBOARD_DATA_PORT    0x60
#define KEYBOARD_STATUS_PORT  0x64

// Function declarations
void init_keyboard(void);
void keyboard_handler(void);
int keyboard_has_char(void);
int keyboard_try_read_char(char *out_char);
char keyboard_getchar(void);
ssize_t keyboard_get_line(char *buffer, size_t buffer_size);
ssize_t get_line(char *buffer, size_t buffer_size);

#endif /* KEYBOARD_H */