#include "kmalloc.h"
#include "pmm.h"
#include "paging.h"
#include "panic.h"

/* External symbols from linker */
extern uint32_t kernel_end;

/* Block header structure */
typedef struct block_header {
    size_t size;                    /* Size of the block (excluding header) */
    uint8_t free;                   /* 1 if block is free, 0 if allocated */
    struct block_header *next;      /* Next block in list */
    struct block_header *prev;      /* Previous block in list */
    uint32_t magic;                 /* Magic number for validation */
} block_header_t;

/* Magic number for valid blocks */
#define BLOCK_MAGIC     0xDEADBEEF

/* Minimum block size (to avoid tiny fragments) */
#define MIN_BLOCK_SIZE  16

/* Alignment for allocations */
#define ALIGNMENT       8
#define ALIGN(size)     (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

/* Header size (aligned) */
#define HEADER_SIZE     ALIGN(sizeof(block_header_t))

/* Heap boundaries */
static uint32_t heap_start = 0;
static uint32_t heap_end = 0;
static uint32_t heap_max = 0;

/* Head of the block list */
static block_header_t *block_list = NULL;

/* Statistics */
static size_t total_allocations = 0;
static size_t total_frees = 0;

/* External functions */
extern void terminal_write(const char *str);

/* Simple memset - used for zeroing allocated memory */
static void *kmalloc_memset(void *ptr, int value, size_t size) __attribute__((unused));
static void *kmalloc_memset(void *ptr, int value, size_t size)
{
    uint8_t *p = (uint8_t*)ptr;
    while (size--) {
        *p++ = (uint8_t)value;
    }
    return ptr;
}

/* Initialize the kernel heap */
void kmalloc_init(void)
{
    /* Place heap after kernel_end + some space for page tables etc */
    /* Start at a 4KB aligned address */
    heap_start = PAGE_ALIGN_UP((uint32_t)&kernel_end + 0x10000);
    heap_end = heap_start;

    /* Set max heap to 8MB after start (within our identity-mapped region) */
    heap_max = heap_start + (8 * 1024 * 1024);

    block_list = NULL;
    total_allocations = 0;
    total_frees = 0;
}

/* Extend the heap */
void *kbrk(int increment)
{
    uint32_t old_end = heap_end;

    if (increment == 0) {
        return (void*)heap_end;
    }

    if (increment > 0) {
        uint32_t new_end = heap_end + increment;

        /* Check bounds */
        if (new_end > heap_max) {
            warn("kbrk: Heap would exceed maximum size");
            return (void*)-1;
        }

        /* The memory is already identity-mapped in paging_init */
        /* Just extend the logical heap boundary */
        heap_end = new_end;
    } else {
        /* Shrinking the heap */
        uint32_t new_end = heap_end + increment;  /* increment is negative */
        if (new_end < heap_start) {
            new_end = heap_start;
        }
        heap_end = new_end;
    }

    return (void*)old_end;
}

/* Find a free block of sufficient size */
static block_header_t *find_free_block(size_t size)
{
    block_header_t *current = block_list;

    while (current) {
        if (current->free && current->size >= size) {
            return current;
        }
        current = current->next;
    }

    return NULL;
}

/* Split a block if it's too large */
static void split_block(block_header_t *block, size_t size)
{
    /* Only split if remaining space is enough for header + MIN_BLOCK_SIZE */
    if (block->size >= size + HEADER_SIZE + MIN_BLOCK_SIZE) {
        block_header_t *new_block = (block_header_t*)((uint8_t*)block + HEADER_SIZE + size);

        new_block->size = block->size - size - HEADER_SIZE;
        new_block->free = 1;
        new_block->magic = BLOCK_MAGIC;
        new_block->next = block->next;
        new_block->prev = block;

        if (block->next) {
            block->next->prev = new_block;
        }

        block->next = new_block;
        block->size = size;
    }
}

/* Coalesce adjacent free blocks */
static void coalesce_blocks(block_header_t *block)
{
    /* Coalesce with next block if free */
    if (block->next && block->next->free) {
        block->size += HEADER_SIZE + block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }

    /* Coalesce with previous block if free */
    if (block->prev && block->prev->free) {
        block->prev->size += HEADER_SIZE + block->size;
        block->prev->next = block->next;
        if (block->next) {
            block->next->prev = block->prev;
        }
    }
}

/* Allocate memory */
void *kmalloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    /* Align size */
    size = ALIGN(size);
    if (size < MIN_BLOCK_SIZE) {
        size = MIN_BLOCK_SIZE;
    }

    /* Try to find a free block */
    block_header_t *block = find_free_block(size);

    if (block) {
        /* Found a free block - split if necessary */
        split_block(block, size);
        block->free = 0;
        total_allocations++;
        return (void*)((uint8_t*)block + HEADER_SIZE);
    }

    /* No free block found - extend heap */
    size_t total_size = HEADER_SIZE + size;
    void *new_memory = kbrk(total_size);

    if (new_memory == (void*)-1) {
        return NULL;  /* Out of memory */
    }

    /* Create new block */
    block = (block_header_t*)new_memory;
    block->size = size;
    block->free = 0;
    block->magic = BLOCK_MAGIC;
    block->next = NULL;
    block->prev = NULL;

    /* Add to list */
    if (block_list == NULL) {
        block_list = block;
    } else {
        /* Find last block */
        block_header_t *last = block_list;
        while (last->next) {
            last = last->next;
        }
        last->next = block;
        block->prev = last;
    }

    total_allocations++;
    return (void*)((uint8_t*)block + HEADER_SIZE);
}

/* Allocate aligned memory */
void *kmalloc_aligned(size_t size, size_t alignment)
{
    /* Allocate extra space for alignment */
    void *ptr = kmalloc(size + alignment);
    if (ptr == NULL) {
        return NULL;
    }

    /* Align the pointer */
    uint32_t addr = (uint32_t)ptr;
    uint32_t aligned = (addr + alignment - 1) & ~(alignment - 1);

    /* Note: This wastes memory but is simple */
    /* A more sophisticated implementation would track the original pointer */
    return (void*)aligned;
}

/* Free memory */
void kfree(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    /* Get block header */
    block_header_t *block = (block_header_t*)((uint8_t*)ptr - HEADER_SIZE);

    /* Validate magic number */
    if (block->magic != BLOCK_MAGIC) {
        panic("kfree: Invalid block (corrupted or double-free)");
        return;
    }

    /* Check if already free */
    if (block->free) {
        warn("kfree: Block already free (double-free attempt)");
        return;
    }

    /* Mark as free */
    block->free = 1;
    total_frees++;

    /* Coalesce with adjacent free blocks */
    coalesce_blocks(block);
}

/* Get size of allocation */
size_t ksize(void *ptr)
{
    if (ptr == NULL) {
        return 0;
    }

    block_header_t *block = (block_header_t*)((uint8_t*)ptr - HEADER_SIZE);

    if (block->magic != BLOCK_MAGIC) {
        return 0;  /* Invalid block */
    }

    return block->size;
}

/* Get heap statistics */
void kmalloc_get_stats(kmalloc_stats_t *stats)
{
    stats->heap_size = heap_end - heap_start;
    stats->used_size = 0;
    stats->free_size = 0;
    stats->num_allocations = 0;
    stats->num_free_blocks = 0;

    block_header_t *current = block_list;
    while (current) {
        if (current->free) {
            stats->free_size += current->size;
            stats->num_free_blocks++;
        } else {
            stats->used_size += current->size + HEADER_SIZE;
            stats->num_allocations++;
        }
        current = current->next;
    }
}

/* Debug: print hex number */
static void print_hex_km(uint32_t num)
{
    char hex_chars[] = "0123456789ABCDEF";
    char str[9];
    str[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        str[i] = hex_chars[num & 0xF];
        num >>= 4;
    }
    terminal_write(str);
}

/* Debug: print decimal number */
static void print_dec_km(uint32_t num)
{
    char buf[12];
    int i = 10;
    buf[11] = '\0';

    if (num == 0) {
        terminal_write("0");
        return;
    }

    while (num > 0 && i >= 0) {
        buf[i--] = '0' + (num % 10);
        num /= 10;
    }

    terminal_write(&buf[i + 1]);
}

/* Debug: dump heap state */
void kmalloc_dump(void)
{
    terminal_write("\n=== Kernel Heap Dump ===\n");
    terminal_write("Heap start: 0x");
    print_hex_km(heap_start);
    terminal_write("\nHeap end: 0x");
    print_hex_km(heap_end);
    terminal_write("\nHeap size: ");
    print_dec_km(heap_end - heap_start);
    terminal_write(" bytes\n\n");

    terminal_write("Blocks:\n");
    block_header_t *current = block_list;
    int block_num = 0;

    while (current) {
        terminal_write("  [");
        print_dec_km(block_num++);
        terminal_write("] addr=0x");
        print_hex_km((uint32_t)current);
        terminal_write(" size=");
        print_dec_km(current->size);
        terminal_write(" ");
        terminal_write(current->free ? "FREE" : "USED");
        terminal_write("\n");
        current = current->next;
    }

    terminal_write("\nTotal allocations: ");
    print_dec_km(total_allocations);
    terminal_write(", Total frees: ");
    print_dec_km(total_frees);
    terminal_write("\n");
}
