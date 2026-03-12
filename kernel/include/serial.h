#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

void serial_init(void);
int serial_is_enabled(void);
int serial_has_data(void);
char serial_read_char(void);
void serial_write_char(char c);
void serial_write(const char *str);

#endif /* SERIAL_H */