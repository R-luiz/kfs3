#ifndef SIGNALS_H
#define SIGNALS_H

#include "types.h"

#define MAX_KERNEL_SIGNALS          32
#define KERNEL_SIGNAL_TEST          1
#define KERNEL_SIGNAL_KEYBOARD      2
#define KERNEL_SIGNAL_PANIC         3

typedef void (*signal_callback_t)(int signal, uint32_t value, void *context);

void signals_init(void);
int register_signal_handler(int signal, signal_callback_t handler);
int schedule_signal(int signal, uint32_t value, void *context);
int signal_dispatch_pending(void);
uint32_t signal_pending_count(void);

#endif /* SIGNALS_H */