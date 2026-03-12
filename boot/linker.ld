ENTRY(_start)

SECTIONS
{
    /* Kernel starts at 1M */
    . = 1M;

    /* Mark the start of the kernel */
    kernel_start = .;

    /* Multiboot header must be in the first 8KB of the file */
    .multiboot BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
    }

    .text BLOCK(4K) : ALIGN(4K)
    {
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

    /* Mark the end of the kernel - used by memory manager */
    . = ALIGN(4K);
    kernel_end = .;
}
