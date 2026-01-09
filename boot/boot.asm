; Multiboot header constants
MBALIGN     equ 1<<0
MEMINFO     equ 1<<1
FLAGS       equ MBALIGN | MEMINFO
MAGIC       equ 0x1BADB002
CHECKSUM    equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .bss
align 16
stack_bottom:
    resb 16384 ; 16 KiB
stack_top:

section .text
global _start
extern kernel_main

_start:
    ; Setup stack
    mov esp, stack_top

    ; EAX contains multiboot magic number (0x2BADB002)
    ; EBX contains pointer to multiboot info structure
    ; Pass both to kernel_main as arguments (C calling convention)
    push ebx        ; Push multiboot info pointer (2nd arg)
    push eax        ; Push multiboot magic (1st arg)

    ; Call kernel
    call kernel_main

    ; Clean up stack (not really necessary since we halt)
    add esp, 8

    ; If kernel returns, halt the CPU
    cli
.hang:
    hlt
    jmp .hang