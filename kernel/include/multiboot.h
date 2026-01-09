#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include "types.h"

/* Multiboot header magic numbers */
#define MULTIBOOT_HEADER_MAGIC      0x1BADB002
#define MULTIBOOT_BOOTLOADER_MAGIC  0x2BADB002

/* Multiboot info flags */
#define MULTIBOOT_INFO_MEMORY       0x00000001  /* mem_lower and mem_upper are valid */
#define MULTIBOOT_INFO_BOOTDEV      0x00000002  /* boot_device is valid */
#define MULTIBOOT_INFO_CMDLINE      0x00000004  /* cmdline is valid */
#define MULTIBOOT_INFO_MODS         0x00000008  /* mods fields are valid */
#define MULTIBOOT_INFO_AOUT_SYMS    0x00000010  /* a.out symbol table valid */
#define MULTIBOOT_INFO_ELF_SHDR     0x00000020  /* ELF section header table valid */
#define MULTIBOOT_INFO_MEM_MAP      0x00000040  /* mmap fields are valid */
#define MULTIBOOT_INFO_DRIVE_INFO   0x00000080  /* drives fields are valid */
#define MULTIBOOT_INFO_CONFIG_TABLE 0x00000100  /* config_table is valid */
#define MULTIBOOT_INFO_BOOT_LOADER  0x00000200  /* boot_loader_name is valid */
#define MULTIBOOT_INFO_APM_TABLE    0x00000400  /* APM table is valid */
#define MULTIBOOT_INFO_VBE_INFO     0x00000800  /* VBE info is valid */

/* Memory map entry types */
#define MULTIBOOT_MEMORY_AVAILABLE  1
#define MULTIBOOT_MEMORY_RESERVED   2
#define MULTIBOOT_MEMORY_ACPI_RECLAIM 3
#define MULTIBOOT_MEMORY_NVS        4
#define MULTIBOOT_MEMORY_BADRAM     5

/* Multiboot info structure */
typedef struct multiboot_info {
    uint32_t flags;             /* Multiboot info flags */

    /* Available memory from BIOS (in KB) */
    uint32_t mem_lower;         /* Amount of lower memory (< 1MB) */
    uint32_t mem_upper;         /* Amount of upper memory (> 1MB) */

    /* Boot device */
    uint32_t boot_device;

    /* Command line */
    uint32_t cmdline;

    /* Boot modules */
    uint32_t mods_count;
    uint32_t mods_addr;

    /* Symbol table info (union, not used here) */
    uint32_t syms[4];

    /* Memory map */
    uint32_t mmap_length;       /* Length of memory map buffer */
    uint32_t mmap_addr;         /* Address of memory map buffer */

    /* Drive info */
    uint32_t drives_length;
    uint32_t drives_addr;

    /* ROM configuration table */
    uint32_t config_table;

    /* Boot loader name */
    uint32_t boot_loader_name;

    /* APM table */
    uint32_t apm_table;

    /* VBE info */
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
} __attribute__((packed)) multiboot_info_t;

/* Memory map entry structure */
typedef struct multiboot_mmap_entry {
    uint32_t size;              /* Size of this entry (not including size field itself) */
    uint32_t addr_low;          /* Low 32 bits of base address */
    uint32_t addr_high;         /* High 32 bits of base address */
    uint32_t len_low;           /* Low 32 bits of length */
    uint32_t len_high;          /* High 32 bits of length */
    uint32_t type;              /* Type of memory region */
} __attribute__((packed)) multiboot_mmap_entry_t;

/* Helper macros */
#define MMAP_ENTRY_SIZE(entry) ((entry)->size + sizeof((entry)->size))
#define MMAP_ENTRY_NEXT(entry) ((multiboot_mmap_entry_t*)((uint32_t)(entry) + MMAP_ENTRY_SIZE(entry)))

/* Get total available RAM in bytes */
static inline uint32_t multiboot_get_total_memory(multiboot_info_t *mbi)
{
    /* mem_upper is in KB and starts from 1MB, mem_lower is also in KB */
    return ((mbi->mem_upper + 1024) * 1024);
}

#endif /* MULTIBOOT_H */
