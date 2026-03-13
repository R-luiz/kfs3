#ifndef PAGING_H
#define PAGING_H

#include "types.h"

/* Page directory/table entry flags */
#define PAGE_PRESENT        0x001   /* Page is present in memory */
#define PAGE_WRITE          0x002   /* Page is writable */
#define PAGE_USER           0x004   /* Page is accessible from user mode */
#define PAGE_WRITETHROUGH   0x008   /* Write-through caching */
#define PAGE_NOCACHE        0x010   /* Disable caching */
#define PAGE_ACCESSED       0x020   /* Page has been accessed */
#define PAGE_DIRTY          0x040   /* Page has been written to */
#define PAGE_SIZE_4MB       0x080   /* 4MB page (only in page directory) */
#define PAGE_GLOBAL         0x100   /* Global page */

/* Common flag combinations */
#define PAGE_KERNEL         (PAGE_PRESENT | PAGE_WRITE)
#define PAGE_KERNEL_RO      (PAGE_PRESENT)
#define PAGE_USER_RO        (PAGE_PRESENT | PAGE_USER)
#define PAGE_USER_RW        (PAGE_PRESENT | PAGE_WRITE | PAGE_USER)

/* Page directory has 1024 entries */
#define PAGE_DIR_ENTRIES    1024

/* Page table has 1024 entries */
#define PAGE_TABLE_ENTRIES  1024

/* Extract indices from virtual address */
#define PAGE_DIR_INDEX(addr)    (((addr) >> 22) & 0x3FF)
#define PAGE_TABLE_INDEX(addr)  (((addr) >> 12) & 0x3FF)
#define PAGE_OFFSET(addr)       ((addr) & 0xFFF)

/* Get physical address from page entry */
#define PAGE_FRAME(entry)       ((entry) & 0xFFFFF000)

/* Page directory entry type */
typedef uint32_t page_dir_entry_t;

/* Page table entry type */
typedef uint32_t page_table_entry_t;

/* Page directory structure */
typedef struct page_directory {
    page_dir_entry_t entries[PAGE_DIR_ENTRIES];
} __attribute__((aligned(4096))) page_directory_t;

/* Page table structure */
typedef struct page_table {
    page_table_entry_t entries[PAGE_TABLE_ENTRIES];
} __attribute__((aligned(4096))) page_table_t;

/* Initialize paging with identity mapping for kernel */
void paging_init(void);

/* Enable paging */
void paging_enable(void);

/* Disable paging */
void paging_disable(void);

/* Map a virtual address to a physical address */
void paging_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);

/* Map a page in a specific page directory */
void paging_map_page_in_directory(page_directory_t *directory, uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);

/* Unmap a virtual address */
void paging_unmap_page(uint32_t virt_addr);

/* Unmap a page in a specific page directory */
void paging_unmap_page_in_directory(page_directory_t *directory, uint32_t virt_addr);

/* Get the physical address for a virtual address */
uint32_t paging_get_physical(uint32_t virt_addr);

/* Get the physical address for a virtual address in a specific page directory */
uint32_t paging_get_physical_in_directory(page_directory_t *directory, uint32_t virt_addr);

/* Check if a page is mapped */
int paging_is_mapped(uint32_t virt_addr);

/* Get the current page directory */
page_directory_t* paging_get_directory(void);

/* Get the kernel page directory */
page_directory_t* paging_get_kernel_directory(void);

/* Clone or copy an existing page directory */
void paging_copy_directory(page_directory_t *destination, const page_directory_t *source);

/* Switch to a specific page directory */
void paging_switch_directory(page_directory_t *directory);

/* Flush TLB for a specific address */
void paging_flush_tlb(uint32_t addr);

/* Flush entire TLB */
void paging_flush_tlb_all(void);

/* Identity map a range of physical memory */
void paging_identity_map(uint32_t start, uint32_t end, uint32_t flags);

#endif /* PAGING_H */
