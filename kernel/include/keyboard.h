#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"
#include "io.h"

// Keyboard I/O ports
#define KEYBOARD_DATA_PORT    0x60
#define KEYBOARD_STATUS_PORT  0x64

// Special key codes (above ASCII range, queued as chars)
#define KEY_PAGE_UP    ((char)0x80)
#define KEY_PAGE_DOWN  ((char)0x81)
#define KEY_ARROW_UP   ((char)0x82)
#define KEY_ARROW_DOWN ((char)0x83)

// Function declarations
void init_keyboard(void);
void keyboard_handler(void);
int keyboard_has_char(void);
int keyboard_try_read_char(char *out_char);
char keyboard_getchar(void);
ssize_t keyboard_get_line(char *buffer, size_t buffer_size);
ssize_t get_line(char *buffer, size_t buffer_size);
int keyboard_shift_held(void);

#endif /* KEYBOARD_H */