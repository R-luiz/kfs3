#include "paging.h"
#include "pmm.h"
#include "panic.h"

/* External symbols from linker */
extern uint32_t kernel_end;

/* Kernel page directory - must be page-aligned */
static page_directory_t kernel_page_directory __attribute__((aligned(4096)));

/* Pre-allocated page tables for initial identity mapping (first 16MB) */
static page_table_t kernel_page_tables[4] __attribute__((aligned(4096)));

/* Current page directory */
static page_directory_t *current_directory = NULL;
static page_directory_t *kernel_directory = NULL;

/* Simple memset implementation */
static void *memset_paging(void *ptr, int value, size_t size)
{
    uint8_t *p = (uint8_t*)ptr;
    while (size--) {
        *p++ = (uint8_t)value;
    }
    return ptr;
}

/* Load page directory into CR3 */
static inline void load_page_directory(uint32_t addr)
{
    asm volatile("mov %0, %%cr3" : : "r"(addr) : "memory");
}

/* Get CR3 value */
static inline uint32_t get_cr3(void)
{
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/* Enable paging by setting bit 31 of CR0 */
void paging_enable(void)
{
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;  /* Set PG bit */
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

/* Disable paging */
void paging_disable(void)
{
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~0x80000000;  /* Clear PG bit */
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

/* Flush TLB for specific address */
void paging_flush_tlb(uint32_t addr)
{
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

/* Flush entire TLB by reloading CR3 */
void paging_flush_tlb_all(void)
{
    uint32_t cr3 = get_cr3();
    load_page_directory(cr3);
}

/* Initialize paging */
void paging_init(void)
{
    /* Clear page directory */
    memset_paging(&kernel_page_directory, 0, sizeof(page_directory_t));

    /* Clear pre-allocated page tables */
    for (int i = 0; i < 4; i++) {
        memset_paging(&kernel_page_tables[i], 0, sizeof(page_table_t));
    }

    /* Identity map first 16MB (4 page tables) */
    /* This covers: BIOS, VGA, kernel, and initial heap space */
    for (int i = 0; i < 4; i++) {
        /* Fill page table with identity mapping */
        for (int j = 0; j < PAGE_TABLE_ENTRIES; j++) {
            uint32_t phys_addr = (i * PAGE_TABLE_ENTRIES + j) * PAGE_SIZE;
            kernel_page_tables[i].entries[j] = phys_addr | PAGE_KERNEL;
        }

        /* Add page table to page directory */
        kernel_page_directory.entries[i] = (uint32_t)&kernel_page_tables[i] | PAGE_KERNEL;
    }

    /* Set current directory */
    current_directory = &kernel_page_directory;
    kernel_directory = &kernel_page_directory;

    /* Load page directory and enable paging */
    load_page_directory((uint32_t)&kernel_page_directory);
    paging_enable();
}

void paging_copy_directory(page_directory_t *destination, const page_directory_t *source)
{
    if (destination == NULL || source == NULL) {
        return;
    }

    memset_paging(destination, 0, sizeof(page_directory_t));
    for (int i = 0; i < PAGE_DIR_ENTRIES; i++) {
        destination->entries[i] = source->entries[i];
    }
}

static page_table_t *paging_get_or_create_table(page_directory_t *directory, uint32_t virt_addr, uint32_t flags)
{
    uint32_t dir_idx = PAGE_DIR_INDEX(virt_addr);
    page_table_t *table;

    if (!(directory->entries[dir_idx] & PAGE_PRESENT)) {
        uint32_t table_phys = pmm_alloc_frame();
        if (table_phys == 0) {
            panic("paging_map_page: Out of memory for page table");
            return NULL;
        }

        table = (page_table_t *)table_phys;
        memset_paging(table, 0, sizeof(page_table_t));

        directory->entries[dir_idx] = table_phys | PAGE_PRESENT | PAGE_WRITE;
        if (flags & PAGE_USER) {
            directory->entries[dir_idx] |= PAGE_USER;
        }
    } else {
        if (flags & PAGE_USER) {
            directory->entries[dir_idx] |= PAGE_USER;
        }
        table = (page_table_t *)PAGE_FRAME(directory->entries[dir_idx]);
    }

    return table;
}

void paging_map_page_in_directory(page_directory_t *directory, uint32_t virt_addr, uint32_t phys_addr, uint32_t flags)
{
    uint32_t table_idx = PAGE_TABLE_INDEX(virt_addr);
    page_table_t *table;

    if (directory == NULL) {
        return;
    }

    table = paging_get_or_create_table(directory, virt_addr, flags);
    if (table == NULL) {
        return;
    }

    table->entries[table_idx] = (phys_addr & 0xFFFFF000) | (flags & 0xFFF);

    if (directory == current_directory) {
        paging_flush_tlb(virt_addr);
    }
}

/* Map a virtual address to a physical address */
void paging_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags)
{
    paging_map_page_in_directory(current_directory, virt_addr, phys_addr, flags);
}

void paging_unmap_page_in_directory(page_directory_t *directory, uint32_t virt_addr)
{
    uint32_t dir_idx = PAGE_DIR_INDEX(virt_addr);
    uint32_t table_idx = PAGE_TABLE_INDEX(virt_addr);
    page_table_t *table;

    if (directory == NULL || !(directory->entries[dir_idx] & PAGE_PRESENT)) {
        return;
    }

    table = (page_table_t *)PAGE_FRAME(directory->entries[dir_idx]);
    table->entries[table_idx] = 0;

    if (directory == current_directory) {
        paging_flush_tlb(virt_addr);
    }
}

/* Unmap a virtual address */
void paging_unmap_page(uint32_t virt_addr)
{
    paging_unmap_page_in_directory(current_directory, virt_addr);
}

uint32_t paging_get_physical_in_directory(page_directory_t *directory, uint32_t virt_addr)
{
    uint32_t dir_idx = PAGE_DIR_INDEX(virt_addr);
    uint32_t table_idx = PAGE_TABLE_INDEX(virt_addr);
    uint32_t offset = PAGE_OFFSET(virt_addr);
    page_table_t *table;

    if (directory == NULL || !(directory->entries[dir_idx] & PAGE_PRESENT)) {
        return 0;
    }

    table = (page_table_t *)PAGE_FRAME(directory->entries[dir_idx]);
    if (!(table->entries[table_idx] & PAGE_PRESENT)) {
        return 0;
    }

    return PAGE_FRAME(table->entries[table_idx]) | offset;
}

/* Get physical address for virtual address */
uint32_t paging_get_physical(uint32_t virt_addr)
{
    return paging_get_physical_in_directory(current_directory, virt_addr);
}

/* Check if a page is mapped */
int paging_is_mapped(uint32_t virt_addr)
{
    uint32_t dir_idx = PAGE_DIR_INDEX(virt_addr);
    uint32_t table_idx = PAGE_TABLE_INDEX(virt_addr);

    if (!(current_directory->entries[dir_idx] & PAGE_PRESENT)) {
        return 0;
    }

    page_table_t *table = (page_table_t*)PAGE_FRAME(current_directory->entries[dir_idx]);
    return (table->entries[table_idx] & PAGE_PRESENT) != 0;
}

/* Get current page directory */
page_directory_t* paging_get_directory(void)
{
    return current_directory;
}

page_directory_t* paging_get_kernel_directory(void)
{
    return kernel_directory;
}

void paging_switch_directory(page_directory_t *directory)
{
    if (directory == NULL) {
        return;
    }

    current_directory = directory;
    load_page_directory((uint32_t)directory);
}

/* Identity map a range */
void paging_identity_map(uint32_t start, uint32_t end, uint32_t flags)
{
    start = PAGE_ALIGN_DOWN(start);
    end = PAGE_ALIGN_UP(end);

    for (uint32_t addr = start; addr < end; addr += PAGE_SIZE) {
        paging_map_page(addr, addr, flags);
    }
}
