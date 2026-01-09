#ifndef TYPES_H
#define TYPES_H

typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef signed long int64_t;

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long size_t;

typedef int32_t ssize_t;

#define NULL ((void*)0)

/* Boolean type - avoid redefining if compiler provides it */
#ifndef __bool_true_false_are_defined
#define true 1
#define false 0
#endif

#endif /* TYPES_H */