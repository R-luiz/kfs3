#ifndef VMALLOC_H
#define VMALLOC_H

#include "types.h"

/* Initialize virtual memory allocator */
void vmalloc_init(void);

/* Allocate virtual memory region */
void *vmalloc(size_t size);

/* Free virtual memory region */
void vfree(void *ptr);

/* Get size of virtual allocation */
size_t vsize(void *ptr);

/* Extend virtual heap (like sbrk) */
void *vbrk(int increment);

/* Virtual memory statistics */
typedef struct vmalloc_stats {
    size_t total_virtual;       /* Total virtual address space allocated */
    size_t used_virtual;        /* Used virtual memory */
    size_t num_regions;         /* Number of allocated regions */
} vmalloc_stats_t;

void vmalloc_get_stats(vmalloc_stats_t *stats);

/* Debug: dump vmalloc state */
void vmalloc_dump(void);

#endif /* VMALLOC_H */
