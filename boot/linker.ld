ENTRY(_start)

SECTIONS
{
    /* Place GDT at physical address 0x00000800, as required */
    .gdt 0x800 : ALIGN(8) {
       *(.gdt)
    }
    
    /* Your kernel proper starts at 1M */
    . = 1M;

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }
}
