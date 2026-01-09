# KFS-3: Memory Management

42 School - Kernel From Scratch, Third Subject

## Overview

KFS-3 implements a complete memory management system for a bare-metal i386 kernel, including:

- **Memory Paging** - Virtual to physical address translation
- **Physical Memory Manager (PMM)** - Bitmap-based page frame allocator
- **Kernel Heap** - `kmalloc`/`kfree` implementation
- **Virtual Memory Allocator** - `vmalloc`/`vfree` implementation
- **Kernel Panic Handler** - Error handling with visual feedback
- **Interactive Shell** - With memory debugging commands
- **VGA Terminal** - With scrolling, hardware cursor, and backspace support

## Features

### Memory Paging
- Page Directory and Page Tables (4KB pages)
- Identity mapping for kernel space (first 16MB)
- CR3 register management
- TLB flushing support

### Physical Memory Manager
- Bitmap-based page frame allocator
- Parses multiboot memory map
- Supports up to 128MB RAM
- Functions: `pmm_alloc_frame()`, `pmm_free_frame()`, `pmm_alloc_frames()`

### Kernel Heap (kmalloc)
- Simple linked-list allocator
- First-fit allocation with block splitting
- Block coalescing on free
- Functions: `kmalloc()`, `kfree()`, `ksize()`, `kbrk()`

### Virtual Memory Allocator
- Page-granularity allocations
- Maps physical pages to virtual addresses
- Functions: `vmalloc()`, `vfree()`, `vsize()`, `vbrk()`

### Kernel Panic
- Fatal panics (red screen, system halt)
- Non-fatal warnings (continue execution)
- Assert macros for debugging

## Requirements

- GCC with 32-bit support (`gcc` and `lib32-glibc` on Arch)
- NASM assembler
- LD (GNU linker)
- QEMU (`qemu-system-x86`)

### Install on Arch Linux
```bash
sudo pacman -S gcc nasm qemu-system-x86
```

### Install on Ubuntu/Debian
```bash
sudo apt install gcc nasm qemu-system-x86 gcc-multilib
```

## Building & Running

### Quick Start
```bash
make        # Build the kernel
make run    # Run in QEMU
```

### All Make Targets
| Command | Description |
|---------|-------------|
| `make` | Build kernel.bin |
| `make run` | Build and run in QEMU (direct multiboot) |
| `make image` | Create bootable disk image (os.img) |
| `make run-image` | Run from disk image (requires GRUB) |
| `make clean` | Remove build artifacts |

### Running Manually
```bash
# Direct multiboot (recommended)
qemu-system-i386 -kernel kernel.bin -m 128M

# With curses display (text mode)
qemu-system-i386 -kernel kernel.bin -m 128M -curses

# From disk image
qemu-system-i386 -hda os.img -m 128M
```

## Usage

When the kernel boots, you'll see:
1. ASCII art header
2. Memory initialization messages
3. Shell prompt: `shell>`

### Example Session
```
shell> meminfo

=== Physical Memory ===
Total: 128 MB
Used:  4 MB (1024 pages)
Free:  124 MB (31744 pages)

=== Kernel Heap ===
Heap size: 0 bytes
Used:      0 bytes
Free:      0 bytes
Allocs:    0

=== Virtual Memory ===
Allocated: 0 bytes
Regions:   0

shell> alloc 1024
Allocated 1024 bytes at 0x00110038

shell> alloc 2048
Allocated 2048 bytes at 0x00110448

shell> kheap

=== Kernel Heap Dump ===
Heap start: 0x00110000
Heap end: 0x00110C50
Heap size: 3152 bytes

Blocks:
  [0] addr=0x00110000 size=1024 USED
  [1] addr=0x00110428 size=2048 USED

shell> free
Freeing 2048 bytes at 0x00110448

shell> meminfo
...

shell> panic
Triggering test kernel panic...
```

The `panic` command will show a red kernel panic screen and halt the system.

### Debug Session Example
```
shell> hexdump 0xb8000 32
Memory dump at 0x000B8000 (32 bytes):
0x000B8000: 41 07 4E 07 54 07 48 07  52 07 4F 07 44 07 52 07  |ANTHRODR|
0x000B8010: 00 07 00 07 00 07 00 07  00 07 00 07 00 07 00 07  |........|

shell> peek 0x100000
Value at 0x00100000: 0x464C457F (1179403647)

shell> stack
=== Stack Dump ===
ESP: 0x0010FFB0  EBP: 0x0010FFD0

Stack contents (64 bytes from ESP):
0x0010FFB0: 00 00 00 00 10 00 00 00 ...

shell> pagedir
=== Page Directory ===
Address: 0x00109000

Mapped page tables:
  [0] -> 0x0010A000 (RW,K) VA: 0x00000000-0x003FFFFF
  [1] -> 0x0010B000 (RW,K) VA: 0x00400000-0x007FFFFF
  ...
```

## Shell Commands

### Basic Commands
| Command | Description |
|---------|-------------|
| `help` | Show available commands |
| `echo TEXT` | Print TEXT to screen |
| `clear` | Clear screen |
| `exit` | Exit shell (halts system) |
| `reboot` | Reboot the system |

### Memory Commands
| Command | Description |
|---------|-------------|
| `meminfo` | Display memory statistics (PMM, heap, vmalloc) |
| `alloc SIZE` | Allocate SIZE bytes with kmalloc |
| `free` | Free last allocation |
| `kheap` | Dump kernel heap state |
| `vmap` | Show virtual memory mappings |

### Debug Commands (Bonus)
| Command | Description |
|---------|-------------|
| `hexdump ADDR [SIZE]` | Dump memory at ADDR in hex+ASCII (default 64 bytes, max 512) |
| `peek ADDR` | Read and display 32-bit value at address |
| `poke ADDR VALUE` | Write 32-bit VALUE to memory address |
| `stack` | Dump current stack (ESP, EBP, 64 bytes of stack) |
| `pagedir` | Show page directory entries with permissions |
| `panic` | Trigger test kernel panic |

## Project Structure

```
kfs3/
├── boot/
│   ├── boot.asm          # Multiboot entry point
│   └── linker.ld         # Linker script
├── kernel/
│   ├── include/
│   │   ├── types.h       # Basic type definitions
│   │   ├── vga.h         # VGA text mode
│   │   ├── gdt.h         # Global Descriptor Table
│   │   ├── keyboard.h    # Keyboard I/O
│   │   ├── shell.h       # Shell interface
│   │   ├── multiboot.h   # Multiboot structures
│   │   ├── pmm.h         # Physical Memory Manager
│   │   ├── paging.h      # Paging structures
│   │   ├── kmalloc.h     # Kernel heap
│   │   ├── vmalloc.h     # Virtual memory
│   │   └── panic.h       # Panic handler
│   └── src/
│       ├── main.c        # Kernel entry point
│       ├── gdt.c         # GDT initialization
│       ├── keyboard.c    # Keyboard driver
│       ├── shell.c       # Interactive shell
│       ├── panic.c       # Panic implementation
│       ├── pmm.c         # PMM implementation
│       ├── paging.c      # Paging implementation
│       ├── kmalloc.c     # Heap allocator
│       └── vmalloc.c     # VMalloc implementation
├── tools/
│   └── create_image.sh   # Bootable image script
├── Makefile
└── README.md
```

## Memory Layout

```
0x00000000 - 0x000FFFFF   Reserved (BIOS, VGA buffer, etc.)
0x00100000 - kernel_end   Kernel code and data
kernel_end - 0x01000000   Kernel heap (kmalloc)
0x01000000 - 0x10000000   Virtual memory area (vmalloc)
```

## Technical Details

- **Architecture**: i386 (x86 32-bit)
- **Page Size**: 4KB
- **Max RAM**: 128MB (configurable)
- **Heap Max**: 8MB
- **Boot**: Multiboot specification with GRUB

## API Reference

### Physical Memory Manager
```c
void pmm_init(multiboot_info_t *mbi);
uint32_t pmm_alloc_frame(void);
void pmm_free_frame(uint32_t addr);
uint32_t pmm_alloc_frames(uint32_t count);
```

### Paging
```c
void paging_init(void);
void paging_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
void paging_unmap_page(uint32_t virt);
uint32_t paging_get_physical(uint32_t virt);
```

### Kernel Heap
```c
void *kmalloc(size_t size);
void kfree(void *ptr);
size_t ksize(void *ptr);
void *kbrk(int increment);
```

### Virtual Memory
```c
void *vmalloc(size_t size);
void vfree(void *ptr);
size_t vsize(void *ptr);
void *vbrk(int increment);
```

### Panic
```c
void panic(const char *message);
void warn(const char *message);
ASSERT(condition);
KASSERT(condition, message);
```

## Bonus Features

This implementation includes bonus features for memory debugging:

- **Memory Dumping** (`hexdump`) - Hex + ASCII dump of any memory region
- **Memory Read/Write** (`peek`/`poke`) - Direct memory access for debugging
- **Stack Inspection** (`stack`) - View current stack pointer and contents
- **Page Table Debug** (`pagedir`) - Inspect page directory entries with permissions
- **System Reboot** (`reboot`) - Soft reboot via keyboard controller

These tools are useful for:
- Debugging memory allocations
- Inspecting VGA buffer (0xB8000)
- Verifying page table mappings
- Examining kernel data structures
- Testing memory protection

## Author

Anthrodr - 42 School

## License

Educational project for 42 School curriculum.
