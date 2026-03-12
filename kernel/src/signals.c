#include "signals.h"
#include "io.h"

#define SIGNAL_QUEUE_CAPACITY  64

typedef struct pending_signal {
    uint8_t signal;
    uint32_t value;
    void *context;
} pending_signal_t;

static signal_callback_t signal_handlers[MAX_KERNEL_SIGNALS];
static pending_signal_t pending_signals[SIGNAL_QUEUE_CAPACITY];
static volatile uint32_t signal_head = 0;
static volatile uint32_t signal_tail = 0;

void signals_init(void)
{
    uint32_t index;

    for (index = 0; index < MAX_KERNEL_SIGNALS; index++) {
        signal_handlers[index] = NULL;
    }
    signal_head = 0;
    signal_tail = 0;
}

int register_signal_handler(int signal, signal_callback_t handler)
{
    uint32_t flags;

    if (signal <= 0 || signal >= MAX_KERNEL_SIGNALS) {
        return -1;
    }

    flags = interrupts_save();
    signal_handlers[signal] = handler;
    interrupts_restore(flags);
    return 0;
}

int schedule_signal(int signal, uint32_t value, void *context)
{
    uint32_t flags;
    uint32_t next_tail;

    if (signal <= 0 || signal >= MAX_KERNEL_SIGNALS) {
        return -1;
    }

    flags = interrupts_save();
    next_tail = (signal_tail + 1) % SIGNAL_QUEUE_CAPACITY;
    if (next_tail == signal_head) {
        interrupts_restore(flags);
        return -1;
    }

    pending_signals[signal_tail].signal = (uint8_t)signal;
    pending_signals[signal_tail].value = value;
    pending_signals[signal_tail].context = context;
    signal_tail = next_tail;
    interrupts_restore(flags);
    return 0;
}

int signal_dispatch_pending(void)
{
    int dispatched = 0;

    while (1) {
        pending_signal_t pending;
        signal_callback_t callback;
        uint32_t flags = interrupts_save();

        if (signal_head == signal_tail) {
            interrupts_restore(flags);
            break;
        }

        pending = pending_signals[signal_head];
        signal_head = (signal_head + 1) % SIGNAL_QUEUE_CAPACITY;
        callback = signal_handlers[pending.signal];
        interrupts_restore(flags);

        if (callback != NULL) {
            callback((int)pending.signal, pending.value, pending.context);
        }
        dispatched++;
    }

    return dispatched;
}

uint32_t signal_pending_count(void)
{
    uint32_t flags = interrupts_save();
    uint32_t count;

    if (signal_tail >= signal_head) {
        count = signal_tail - signal_head;
    } else {
        count = SIGNAL_QUEUE_CAPACITY - signal_head + signal_tail;
    }

    interrupts_restore(flags);
    return count;
}