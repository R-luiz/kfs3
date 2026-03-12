#include "vmalloc.h"
#include "kmalloc.h"
#include "pmm.h"
#include "paging.h"
#include "panic.h"

/* Virtual memory region descriptor */
typedef struct vm_region {
    uint32_t virt_addr;         /* Virtual start address */
    uint32_t size;              /* Requested size in bytes */
    uint32_t mapped_size;       /* Mapped size in bytes */
    uint32_t num_pages;         /* Number of pages */
    struct vm_region *next;     /* Next region */
    uint32_t magic;             /* Magic number for validation */
} vm_region_t;

/* Magic number */
#define VM_MAGIC    0xCAFEBABE

/* Virtual address space for vmalloc (after identity-mapped region) */
/* We use addresses starting at 16MB since first 16MB is identity-mapped */
#define VMALLOC_START   0x01000000      /* 16 MB */
#define VMALLOC_END     0x10000000      /* 256 MB */

/* Current virtual break used by vmalloc/vbrk. */
static uint32_t vmalloc_current = VMALLOC_START;

/* List of allocated regions */
static vm_region_t *region_list = NULL;

/* Statistics */
static size_t total_regions = 0;
static size_t total_pages_allocated = 0;

/* External functions */
extern void terminal_write(const char *str);

/* Simple memset */
static void *vmalloc_memset(void *ptr, int value, size_t size)
{
    uint8_t *p = (uint8_t*)ptr;
    while (size--) {
        *p++ = (uint8_t)value;
    }
    return ptr;
}

static uint32_t vmalloc_highest_end(void)
{
    uint32_t highest = VMALLOC_START;
    vm_region_t *current = region_list;

    while (current) {
        uint32_t end = current->virt_addr + current->mapped_size;
        if (end > highest) {
            highest = end;
        }
        current = current->next;
    }

    return highest;
}

static vm_region_t *vmalloc_find_region(void *ptr, vm_region_t **prev_region)
{
    vm_region_t *previous = NULL;
    vm_region_t *current = region_list;
    uint32_t virt_addr = (uint32_t)ptr;

    while (current) {
        if (current->magic == VM_MAGIC && current->virt_addr == virt_addr) {
            if (prev_region) {
                *prev_region = previous;
            }
            return current;
        }
        previous = current;
        current = current->next;
    }

    if (prev_region) {
        *prev_region = NULL;
    }
    return NULL;
}

/* Initialize vmalloc */
void vmalloc_init(void)
{
    vmalloc_current = VMALLOC_START;
    region_list = NULL;
    total_regions = 0;
    total_pages_allocated = 0;
}

/* Extend virtual address space (like sbrk) */
void *vbrk(int increment)
{
    uint32_t old_current = vmalloc_current;
    uint32_t min_current = vmalloc_highest_end();

    if (increment == 0) {
        return (void*)vmalloc_current;
    }

    if (increment > 0) {
        uint32_t new_current = vmalloc_current + increment;

        if (new_current > VMALLOC_END) {
            warn("vbrk: Would exceed virtual address space");
            return (void*)-1;
        }

        vmalloc_current = new_current;
    } else {
        uint32_t new_current = vmalloc_current + increment;
        if (new_current < min_current) {
            new_current = min_current;
        }
        vmalloc_current = new_current;
    }

    return (void*)old_current;
}

/* Allocate virtual memory */
void *vmalloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    /* Round up to page boundary */
    uint32_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t alloc_size = num_pages * PAGE_SIZE;

    /* Check if we have enough virtual address space */
    if (vmalloc_current + alloc_size > VMALLOC_END) {
        warn("vmalloc: Out of virtual address space");
        return NULL;
    }

    vm_region_t *region = (vm_region_t*)kmalloc(sizeof(vm_region_t));
    if (region == NULL) {
        warn("vmalloc: Out of kernel memory for metadata");
        return NULL;
    }

    /* Allocate physical pages and map them */
    uint32_t virt_addr = vmalloc_current;

    for (uint32_t i = 0; i < num_pages; i++) {
        uint32_t phys_addr = pmm_alloc_frame();
        if (phys_addr == 0) {
            /* Out of physical memory - unmap what we've mapped so far */
            for (uint32_t j = 0; j < i; j++) {
                uint32_t addr = virt_addr + j * PAGE_SIZE;
                uint32_t phys = paging_get_physical(addr);
                paging_unmap_page(addr);
                pmm_free_frame(phys);
            }
            kfree(region);
            warn("vmalloc: Out of physical memory");
            return NULL;
        }

        /* Map the page */
        paging_map_page(virt_addr + i * PAGE_SIZE, phys_addr, PAGE_KERNEL);

        /* Zero the page */
        vmalloc_memset((void*)(virt_addr + i * PAGE_SIZE), 0, PAGE_SIZE);
    }

    region->virt_addr = virt_addr;
    region->size = (uint32_t)size;
    region->mapped_size = alloc_size;
    region->num_pages = num_pages;
    region->magic = VM_MAGIC;
    region->next = region_list;
    region_list = region;

    /* Update statistics */
    total_regions++;
    total_pages_allocated += num_pages;

    /* Update current pointer */
    vmalloc_current += alloc_size;

    return (void*)virt_addr;
}

/* Free virtual memory */
void vfree(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    vm_region_t *prev_region = NULL;
    vm_region_t *region = vmalloc_find_region(ptr, &prev_region);

    if (region == NULL) {
        panic("vfree: Invalid region (corrupted or double-free)");
        return;
    }

    if (prev_region) {
        prev_region->next = region->next;
    } else {
        region_list = region->next;
    }

    /* Unmap and free physical pages */
    for (uint32_t i = 0; i < region->num_pages; i++) {
        uint32_t vaddr = region->virt_addr + i * PAGE_SIZE;
        uint32_t paddr = paging_get_physical(vaddr);

        if (paddr) {
            paging_unmap_page(vaddr);
            pmm_free_frame(paddr);
        }
    }

    /* Update statistics */
    total_regions--;
    total_pages_allocated -= region->num_pages;

    /* Invalidate magic */
    region->magic = 0;
    kfree(region);
    vmalloc_current = vmalloc_highest_end();
}

/* Get size of virtual allocation */
size_t vsize(void *ptr)
{
    if (ptr == NULL) {
        return 0;
    }

    vm_region_t *region = vmalloc_find_region(ptr, NULL);
    if (region == NULL) {
        return 0;
    }

    return region->size;
}

/* Get statistics */
void vmalloc_get_stats(vmalloc_stats_t *stats)
{
    stats->total_virtual = total_pages_allocated * PAGE_SIZE;
    stats->used_virtual = total_pages_allocated * PAGE_SIZE;
    stats->num_regions = total_regions;
}

/* Debug: print hex number */
static void print_hex_vm(uint32_t num)
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
static void print_dec_vm(uint32_t num)
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

/* Debug: dump vmalloc state */
void vmalloc_dump(void)
{
    terminal_write("\n=== Virtual Memory Dump ===\n");
    terminal_write("VMalloc range: 0x");
    print_hex_vm(VMALLOC_START);
    terminal_write(" - 0x");
    print_hex_vm(vmalloc_current);
    terminal_write("\n");
    terminal_write("Total regions: ");
    print_dec_vm(total_regions);
    terminal_write("\n");
    terminal_write("Total pages: ");
    print_dec_vm(total_pages_allocated);
    terminal_write(" (");
    print_dec_vm(total_pages_allocated * PAGE_SIZE / 1024);
    terminal_write(" KB)\n");

    terminal_write("\nRegions:\n");
    vm_region_t *current = region_list;
    int region_num = 0;

    while (current && current->magic == VM_MAGIC) {
        terminal_write("  [");
        print_dec_vm(region_num++);
        terminal_write("] vaddr=0x");
        print_hex_vm(current->virt_addr);
        terminal_write(" pages=");
        print_dec_vm(current->num_pages);
        terminal_write(" size=");
        print_dec_vm(current->size);
        terminal_write(" mapped=");
        print_dec_vm(current->mapped_size);
        terminal_write("\n");
        current = current->next;
    }
}
