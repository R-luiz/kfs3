#ifndef PANIC_H
#define PANIC_H

#include "types.h"
#include "idt.h"

/* Panic levels */
#define PANIC_FATAL     0   /* Halt the system */
#define PANIC_WARNING   1   /* Print warning, continue */

typedef struct panic_context {
    int valid;
    int has_registers;
    uint32_t saved_stack_pointer;
    uint32_t saved_stack_size;
    cpu_registers_t register_snapshot;
    uint8_t stack_snapshot[128];
} panic_context_t;

/* Fatal kernel panic - halts the system */
void panic(const char *message);

/* Panic with file and line info */
void panic_at(const char *message, const char *file, int line);

/* Panic helpers for interrupts and halt paths */
void panic_with_registers(const char *message, const cpu_registers_t *registers);
void panic_with_context(const char *message, const char *file, int line, const cpu_registers_t *registers);
void panic_save_context(const cpu_registers_t *registers);
void panic_save_stack_snapshot(uint32_t stack_pointer);
const panic_context_t *panic_get_last_context(void);
void panic_clean_registers_and_halt(void) __attribute__((noreturn));

/* Non-fatal warning - prints message but continues */
void warn(const char *message);

/* Assert macro - panics if condition is false */
#define ASSERT(condition) \
    do { \
        if (!(condition)) { \
            panic_at("Assertion failed: " #condition, __FILE__, __LINE__); \
        } \
    } while (0)

/* Kernel assert with custom message */
#define KASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            panic_at(message, __FILE__, __LINE__); \
        } \
    } while (0)

#endif /* PANIC_H */
