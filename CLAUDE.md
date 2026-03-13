# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

KFS (Kernel From Scratch) is a bare-metal x86 32-bit OS kernel built for the 42 School curriculum, covering milestones KFS-2 through KFS-5. It implements memory management, paging, interrupts, processes, and scheduling — all from scratch without any standard library.

- **Architecture:** i386 (x86 32-bit), multiboot-compliant, boots under QEMU/GRUB
- **No stdlib:** `-ffreestanding -nostdlib -fno-builtin`; no SSE/MMX/FPU (`-mno-sse -mno-sse2 -mno-mmx -mno-80387`)
- **Build output:** `kernel.bin`

## Build & Run Commands

```bash
make              # Build kernel.bin
make run          # Build and run in QEMU (auto-detects display backend)
make clean        # Remove all build artifacts
make debug        # Run QEMU with interrupt/CPU-reset tracing
make image        # Create bootable os.img via GRUB
make run-image    # Run from disk image
make check-multiboot  # Validate multiboot header compliance
```

## Testing

```bash
make test-kfs          # Run the full automated test suite (tools/test_kfs.py)
python3 tools/test_kfs.py  # Run directly
```

The test suite (`tools/test_kfs.py`) spawns the kernel in a PTY, sends shell commands, and validates output against expected patterns. It covers: GDT/IDT structure, PMM/paging/heap, process tables, scheduler ticks, fork/wait/kill/getuid, signals, and IPC.

## Architecture & Initialization Order

Kernel entry: `boot/boot.asm` → `kernel/src/main.c::kernel_main(magic, mbi)`

Initialization sequence (order matters — each depends on the previous):
1. **VGA terminal** — text-mode output (80×25)
2. **GDT** — 7 segment descriptors, flat memory model
3. **Serial** — COM1 debug output, mirrors VGA; used for headless testing
4. **Multiboot parsing + PMM** — bitmap page-frame allocator, parses memory map
5. **Paging** — 4KB pages, identity-maps first 16MB for kernel
6. **Kernel heap (kmalloc)** — linked-list allocator above kernel image
7. **vmalloc** — page-granularity allocator above 16MB
8. **IDT + PIC** — 256 vectors; IRQs remapped to vectors 32–47; `int 0x80` = syscall
9. **Signals** — kernel-level callback dispatcher (32 slots)
10. **Syscall** — `int 0x80` handler; implements fork/wait/exit/getuid/signal/kill
11. **Process manager + scheduler** — round-robin, up to 16 processes
12. **Timer (PIT)** — IRQ0 at 50 Hz, drives scheduler ticks
13. **Keyboard** — IRQ1, interrupt-driven PS/2 input with buffer
14. **Shell** — interactive command interpreter (~30 commands)

## Memory Layout

```
0x00000000 – 0x000FFFFF   Reserved (BIOS, VGA at 0xB8000)
0x00100000 – kernel_end   Kernel (.text/.rodata/.data/.bss), loaded by linker.ld
kernel_end – 0x01000000   Kernel heap (kmalloc), up to 8MB
0x01000000 – 0x10000000   Virtual memory area (vmalloc)
0x02000000+               Per-process regions (up to 16 processes)
```

`kernel_start` and `kernel_end` symbols are exported by `boot/linker.ld` and used by PMM to mark kernel pages as used.

## Key Subsystem APIs

**Memory:**
```c
void *kmalloc(size_t size);   void kfree(void *ptr);   // kernel heap
void *vmalloc(size_t size);   void vfree(void *ptr);   // virtual memory
uint32_t pmm_alloc_frame();   void pmm_free_frame(uint32_t addr);
void paging_map_page(virt, phys, flags);
```

**Processes:**
```c
process_t *exec_fn(addr, func, size);   // run kernel function as process
process_t *fork_process(source);
int process_wait(parent, pid, &exit_code);
int kill_process(sender, pid, signal);
int queue_process_signal(proc, sig, val);
```

**Interrupts:**
```c
void idt_register_handler(uint8_t vector, isr_handler_t handler);
void pic_set_mask(uint8_t irq);   // disable IRQ
// All ISRs call interrupt_handler(cpu_registers_t *regs) in interrupts.asm
```

**IPC (socket-style, 8-message queue per process):**
```c
int socket_send(process_t *target, uint32_t value);
int socket_recv(process_t *self, uint32_t *out);
```

## Process Structure

```c
typedef struct process {
    uint32_t pid;
    process_status_t status;         // READY, RUN, WAITING, ZOMBIE, THREAD
    process_memory_t memory;         // page dir, heap, stack, data sections
    process_pending_signal_t signals[16];
    process_socket_t socket;         // IPC message queue
    process_exec_fn_t entry;
    process_signal_handler_t handler;
    process_t *parent, *children, *next;
    uint32_t exit_code;
} process_t;
```

## Shell Commands (for testing/debugging)

The interactive shell (`kernel/src/shell.c`) provides ~30 commands grouped by category:
- **Memory:** `meminfo`, `alloc SIZE`, `free`, `kheap`, `vmap`
- **Debug:** `hexdump ADDR [SIZE]`, `peek ADDR`, `poke ADDR VAL`, `stack`, `pagedir`, `idt`, `signals`, `sigtest`, `syscall`, `int3`, `panic`
- **Process:** `procs`, `execdemo`, `sched TICKS`, `forkdemo PID`, `waitproc PID`, `killproc PID [SIG]`, `sigproc PID SIG [VAL]`, `getuid PID`, `mmapproc PID SIZE`, `sockdemo`

## Assembly Integration

`kernel/src/interrupts.asm` contains 256 ISR stubs (generated via NASM macros) that:
1. Push a `cpu_registers_t` struct onto the stack
2. Call the C dispatcher `interrupt_handler(cpu_registers_t *regs)`

`boot/boot.asm` contains the multiboot header and sets up the initial stack before calling `kernel_main`.
