#include "pmm.h"
#include "panic.h"

/* External symbols from linker */
extern uint32_t kernel_start;
extern uint32_t kernel_end;

/* Bitmap for tracking page frames - 1 bit per frame */
/* For 4GB address space: 4GB / 4KB = 1M frames = 128KB bitmap */
/* We'll allocate space for up to 128MB of RAM = 32K frames = 4KB bitmap */
#define MAX_MEMORY      (128 * 1024 * 1024)  /* 128 MB max */
#define MAX_FRAMES      (MAX_MEMORY / PAGE_SIZE)
#define BITMAP_SIZE     (MAX_FRAMES / 8)

static uint8_t frame_bitmap[BITMAP_SIZE];

/* PMM state */
static uint32_t total_memory = 0;
static uint32_t total_frames = 0;
static uint32_t used_frames = 0;

/* Bitmap manipulation helpers */
static inline void bitmap_set(uint32_t frame)
{
    frame_bitmap[frame / 8] |= (1 << (frame % 8));
}

static inline void bitmap_clear(uint32_t frame)
{
    frame_bitmap[frame / 8] &= ~(1 << (frame % 8));
}

static inline int bitmap_test(uint32_t frame)
{
    return (frame_bitmap[frame / 8] & (1 << (frame % 8))) != 0;
}

/* Find first free frame */
static int32_t bitmap_find_free(void)
{
    for (uint32_t i = 0; i < total_frames / 8; i++) {
        if (frame_bitmap[i] != 0xFF) {
            /* Found a byte with at least one free bit */
            for (uint32_t j = 0; j < 8; j++) {
                uint32_t frame = i * 8 + j;
                if (frame < total_frames && !bitmap_test(frame)) {
                    return frame;
                }
            }
        }
    }
    return -1;  /* No free frame found */
}

/* Find first sequence of n free frames */
static int32_t bitmap_find_free_region(uint32_t count)
{
    uint32_t consecutive = 0;
    uint32_t start_frame = 0;

    for (uint32_t frame = 0; frame < total_frames; frame++) {
        if (!bitmap_test(frame)) {
            if (consecutive == 0) {
                start_frame = frame;
            }
            consecutive++;
            if (consecutive >= count) {
                return start_frame;
            }
        } else {
            consecutive = 0;
        }
    }
    return -1;  /* No free region found */
}

/* Initialize the physical memory manager */
void pmm_init(multiboot_info_t *mbi)
{
    /* Clear the bitmap - all frames start as used */
    for (uint32_t i = 0; i < BITMAP_SIZE; i++) {
        frame_bitmap[i] = 0xFF;
    }

    /* Get total memory from multiboot info */
    if (mbi->flags & MULTIBOOT_INFO_MEMORY) {
        /* mem_upper is in KB, starting from 1MB */
        total_memory = (mbi->mem_upper + 1024) * 1024;
        if (total_memory > MAX_MEMORY) {
            total_memory = MAX_MEMORY;
        }
    } else {
        /* Fallback: assume 16MB */
        total_memory = 16 * 1024 * 1024;
    }

    total_frames = total_memory / PAGE_SIZE;
    used_frames = total_frames;  /* Start with all used, then free available regions */

    /* Parse memory map if available */
    if (mbi->flags & MULTIBOOT_INFO_MEM_MAP) {
        multiboot_mmap_entry_t *mmap = (multiboot_mmap_entry_t*)mbi->mmap_addr;
        multiboot_mmap_entry_t *mmap_end = (multiboot_mmap_entry_t*)(mbi->mmap_addr + mbi->mmap_length);

        while (mmap < mmap_end) {
            if (mmap->type == MULTIBOOT_MEMORY_AVAILABLE) {
                /* Only consider low 32-bits for i386 */
                uint32_t base = mmap->addr_low;
                uint32_t length = mmap->len_low;

                /* Mark available memory as free */
                pmm_mark_region_free(base, length);
            }
            mmap = MMAP_ENTRY_NEXT(mmap);
        }
    } else {
        /* No memory map - assume memory from 1MB to total_memory is available */
        pmm_mark_region_free(0x100000, total_memory - 0x100000);
    }

    /* Mark first 1MB as used (BIOS, VGA, etc.) */
    pmm_mark_region_used(0, 0x100000);

    /* Mark kernel memory as used */
    uint32_t kernel_size = (uint32_t)&kernel_end - (uint32_t)&kernel_start;
    pmm_mark_region_used((uint32_t)&kernel_start, kernel_size);

    /* Reserve some space after kernel for initial structures (64KB) */
    pmm_mark_region_used((uint32_t)&kernel_end, 64 * 1024);
}

/* Allocate a single page frame */
uint32_t pmm_alloc_frame(void)
{
    int32_t frame = bitmap_find_free();
    if (frame < 0) {
        return 0;  /* Out of memory */
    }

    bitmap_set(frame);
    used_frames++;

    return PFN_TO_ADDR(frame);
}

/* Free a page frame */
void pmm_free_frame(uint32_t addr)
{
    uint32_t frame = ADDR_TO_PFN(addr);

    if (frame >= total_frames) {
        return;  /* Invalid frame */
    }

    if (bitmap_test(frame)) {
        bitmap_clear(frame);
        used_frames--;
    }
}

/* Allocate multiple contiguous page frames */
uint32_t pmm_alloc_frames(uint32_t count)
{
    if (count == 0) {
        return 0;
    }

    int32_t start = bitmap_find_free_region(count);
    if (start < 0) {
        return 0;  /* Not enough contiguous memory */
    }

    for (uint32_t i = 0; i < count; i++) {
        bitmap_set(start + i);
    }
    used_frames += count;

    return PFN_TO_ADDR(start);
}

/* Free multiple contiguous page frames */
void pmm_free_frames(uint32_t addr, uint32_t count)
{
    uint32_t frame = ADDR_TO_PFN(addr);

    for (uint32_t i = 0; i < count; i++) {
        if (frame + i < total_frames && bitmap_test(frame + i)) {
            bitmap_clear(frame + i);
            used_frames--;
        }
    }
}

/* Check if a frame is allocated */
int pmm_is_frame_allocated(uint32_t addr)
{
    uint32_t frame = ADDR_TO_PFN(addr);
    if (frame >= total_frames) {
        return 1;  /* Treat invalid frames as allocated */
    }
    return bitmap_test(frame);
}

/* Mark a frame as used */
void pmm_mark_frame_used(uint32_t addr)
{
    uint32_t frame = ADDR_TO_PFN(addr);
    if (frame < total_frames && !bitmap_test(frame)) {
        bitmap_set(frame);
        used_frames++;
    }
}

/* Mark a region as used */
void pmm_mark_region_used(uint32_t base, uint32_t length)
{
    uint32_t start_frame = ADDR_TO_PFN(base);
    uint32_t end_frame = ADDR_TO_PFN(PAGE_ALIGN_UP(base + length));

    for (uint32_t frame = start_frame; frame < end_frame && frame < total_frames; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            used_frames++;
        }
    }
}

/* Mark a region as free */
void pmm_mark_region_free(uint32_t base, uint32_t length)
{
    uint32_t start_frame = ADDR_TO_PFN(PAGE_ALIGN_UP(base));
    uint32_t end_frame = ADDR_TO_PFN(base + length);

    for (uint32_t frame = start_frame; frame < end_frame && frame < total_frames; frame++) {
        if (bitmap_test(frame)) {
            bitmap_clear(frame);
            used_frames--;
        }
    }
}

/* Get PMM statistics */
void pmm_get_stats(pmm_stats_t *stats)
{
    stats->total_frames = total_frames;
    stats->used_frames = used_frames;
    stats->free_frames = total_frames - used_frames;
    stats->total_memory = total_memory;
    stats->free_memory = (total_frames - used_frames) * PAGE_SIZE;
}

/* Get total memory */
uint32_t pmm_get_total_memory(void)
{
    return total_memory;
}

/* Get free memory */
uint32_t pmm_get_free_memory(void)
{
    return (total_frames - used_frames) * PAGE_SIZE;
}
