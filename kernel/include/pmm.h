#ifndef PMM_H
#define PMM_H

#include "types.h"
#include "multiboot.h"

/* Page size is 4KB */
#define PAGE_SIZE       4096
#define PAGE_SHIFT      12

/* Macros for page alignment */
#define PAGE_ALIGN_DOWN(addr)   ((addr) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(addr)     (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

/* Convert address to page frame number and vice versa */
#define ADDR_TO_PFN(addr)       ((addr) >> PAGE_SHIFT)
#define PFN_TO_ADDR(pfn)        ((pfn) << PAGE_SHIFT)

/* PMM statistics */
typedef struct pmm_stats {
    uint32_t total_frames;      /* Total number of page frames */
    uint32_t used_frames;       /* Number of used page frames */
    uint32_t free_frames;       /* Number of free page frames */
    uint32_t total_memory;      /* Total memory in bytes */
    uint32_t free_memory;       /* Free memory in bytes */
} pmm_stats_t;

/* Initialize the physical memory manager */
void pmm_init(multiboot_info_t *mbi);

/* Allocate a single page frame (returns physical address or 0 on failure) */
uint32_t pmm_alloc_frame(void);

/* Free a page frame */
void pmm_free_frame(uint32_t addr);

/* Allocate multiple contiguous page frames */
uint32_t pmm_alloc_frames(uint32_t count);

/* Free multiple contiguous page frames */
void pmm_free_frames(uint32_t addr, uint32_t count);

/* Check if a frame is allocated */
int pmm_is_frame_allocated(uint32_t addr);

/* Mark a frame as used (for reserved memory) */
void pmm_mark_frame_used(uint32_t addr);

/* Mark a range of frames as used */
void pmm_mark_region_used(uint32_t base, uint32_t length);

/* Mark a range of frames as free */
void pmm_mark_region_free(uint32_t base, uint32_t length);

/* Get PMM statistics */
void pmm_get_stats(pmm_stats_t *stats);

/* Get total memory in bytes */
uint32_t pmm_get_total_memory(void);

/* Get free memory in bytes */
uint32_t pmm_get_free_memory(void);

#endif /* PMM_H */
