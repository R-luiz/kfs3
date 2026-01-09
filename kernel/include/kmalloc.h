#ifndef KMALLOC_H
#define KMALLOC_H

#include "types.h"

/* Initialize the kernel heap */
void kmalloc_init(void);

/* Allocate memory from kernel heap */
void *kmalloc(size_t size);

/* Allocate aligned memory from kernel heap */
void *kmalloc_aligned(size_t size, size_t alignment);

/* Free memory back to kernel heap */
void kfree(void *ptr);

/* Get size of allocated block */
size_t ksize(void *ptr);

/* Extend/shrink kernel heap (like sbrk) */
void *kbrk(int increment);

/* Get current heap statistics */
typedef struct kmalloc_stats {
    size_t heap_size;       /* Total heap size */
    size_t used_size;       /* Used memory (including headers) */
    size_t free_size;       /* Free memory */
    size_t num_allocations; /* Number of active allocations */
    size_t num_free_blocks; /* Number of free blocks */
} kmalloc_stats_t;

void kmalloc_get_stats(kmalloc_stats_t *stats);

/* Debug: dump heap state */
void kmalloc_dump(void);

#endif /* KMALLOC_H */
