#ifndef PANIC_H
#define PANIC_H

#include "types.h"

/* Panic levels */
#define PANIC_FATAL     0   /* Halt the system */
#define PANIC_WARNING   1   /* Print warning, continue */

/* Fatal kernel panic - halts the system */
void panic(const char *message);

/* Panic with file and line info */
void panic_at(const char *message, const char *file, int line);

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
