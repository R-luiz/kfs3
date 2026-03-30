# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
make                       # Build kernel.bin (in-tree, BUILD_DIR=.)
make BUILD_DIR=build       # Build to build/ directory (out-of-tree)
make run                   # Run in QEMU (auto-detects GTK/SDL/VNC, falls back to serial)
make test-kfs              # Build kernel + run all tests
make test-kfs KFS=3        # Only KFS-3 tests
make test-kfs KFS="2 3"    # KFS-2 and KFS-3 tests
make image                 # Create bootable GRUB disk image
make run-image             # Boot from disk image
make debug                 # Run QEMU with GDB debugging on port 1234
make check-multiboot       # Verify multiboot header in kernel.bin
make clean                 # Remove build artifacts
```

### Test Suite (kfs-testing/)

The test suite lives in a separate `kfs-testing/` repo cloned inside this project. Run via Make:

**Toolchain requirements**: GCC (32-bit), NASM, GNU ld, QEMU (`qemu-system-i386`), Python 3, GRUB 2 (for `make image`).

## Architecture

### Boot Flow
GRUB loads kernel at 0x100000 → [boot/boot.asm](boot/boot.asm) sets up stack and calls `kernel_main` → [kernel/src/main.c](kernel/src/main.c) initializes subsystems in order:

**VGA → GDT → PMM → Paging → IDT/PIC → Keyboard → Timer → Kmalloc → Vmalloc → Processes → Signals → Syscalls → Shell**

### Memory Layout
| Region | Address Range |
|---|---|
| Reserved (BIOS, VGA at 0xB8000) | `0x00000000 – 0x000FFFFF` |
| Kernel code/data/BSS | `0x00100000 – kernel_end` |
| Kernel heap (kmalloc) | `kernel_end – 0x01000000` |
| Virtual memory (vmalloc) | `0x01000000 – 0x10000000` |
| Per-process mmap regions | `0x02000000+` (within vmalloc range, tracked separately) |

Key constants: `PAGE_SIZE=4KB`, `MAX_PROCESSES=16`, `QEMU_MEMORY=128MB`, timer at `50 Hz` (IRQ0).

### Subsystem Map
- **[boot/boot.asm](boot/boot.asm)** — Multiboot entry, stack setup
- **[boot/linker.ld](boot/linker.ld)** — Kernel placed at 1MB, exports `kernel_start`/`kernel_end`
- **[kernel/src/gdt.c](kernel/src/gdt.c)** — 7-descriptor GDT: null + kernel code/data/stack + user code/data/stack
- **[kernel/src/idt.c](kernel/src/idt.c)** + **[interrupts.asm](kernel/src/interrupts.asm)** — 256-entry IDT, PIC remapped to IRQ 32–47
- **[kernel/src/pmm.c](kernel/src/pmm.c)** — Bitmap-based physical page frame allocator (parses multiboot memory map)
- **[kernel/src/paging.c](kernel/src/paging.c)** — 4KB pages, identity-maps kernel region, per-process page directories, TLB flushing
- **[kernel/src/kmalloc.c](kernel/src/kmalloc.c)** — First-fit linked-list heap allocator with block coalescing
- **[kernel/src/vmalloc.c](kernel/src/vmalloc.c)** — Page-granularity virtual memory allocator
- **[kernel/src/process.c](kernel/src/process.c)** — Process table, round-robin scheduler, fork/wait/exit, IPC socket queues
- **[kernel/src/process_switch.asm](kernel/src/process_switch.asm)** — Context switch (saves/restores registers)
- **[kernel/src/signals.c](kernel/src/signals.c)** — Signal queue and callback delivery
- **[kernel/src/syscall.c](kernel/src/syscall.c)** — `int 0x80` dispatch: fork, wait, exit, getuid, signal, kill
- **[kernel/src/keyboard.c](kernel/src/keyboard.c)** — IRQ1 driver feeding input buffer
- **[kernel/src/timer.c](kernel/src/timer.c)** — PIT IRQ0 setup, drives scheduler ticks
- **[kernel/src/shell.c](kernel/src/shell.c)** — 30+ interactive commands (meminfo, alloc, fork, exec, signals, hexdump, peek/poke…)
- **[kernel/src/panic.c](kernel/src/panic.c)** — Red-screen panic with register/stack snapshot; `panic()`, `warn()`, `ASSERT()` macros

### Process States
`READY → RUNNING → WAITING → ZOMBIE` (plus `THREAD` for kernel threads). Scheduler is round-robin triggered by IRQ0.

## Development Workflow

### 1. Plan Mode Default
- Enter plan mode for ANY non-trivial task (3+ steps or architectural decisions)
- If something goes sideways, STOP and re-plan immediately — don't keep pushing
- Use plan mode for verification steps, not just building
- Write detailed specs upfront to reduce ambiguity

### 2. Subagent Strategy
- Use subagents liberally to keep main context window clean
- Offload research, exploration, and parallel analysis to subagents
- For complex problems, throw more compute at it via subagents
- One task per subagent for focused execution

### 3. Self-Improvement Loop
- After ANY correction from the user: update `tasks/lessons. md` with the pattern
- Write rules for yourself that prevent the same mistake
- Ruthlessly iterate on these lessons until mistake rate drops
- Review lessons at session start for relevant project

### 4. Verification Before Done
- Never mark a task complete without proving it works
- Diff behavior between main and your changes when relevant
- Ask yourself: "Would a staff engineer approve this?"
- Run tests, check logs, demonstrate correctness

### 5. Demand Elegance (Balanced)
- For non-trivial changes: pause and ask "is there a more elegant way?"
- If a fix feels hacky: "Knowing everything I know now, implement the elegant solution"
- Skip this for simple, obvious fixes — don't over-engineer
- Challenge your own work before presenting it

### 6. Autonomous Bug Fixing
- When given a bug report: just fix it. Don't ask for hand-holding
- Point at logs, errors, failing tests — then resolve them
- Zero context switching required from the user
- Go fix failing CI tests without being told how

## Task Management

1. **Plan First**: Write plan to `tasks/todo.md` with checkable items  
2. **Verify Plan**: Check in before starting implementation  
3. **Track Progress**: Mark items complete as you go  
4. **Explain Changes**: High-level summary at each step  
5. **Document Results**: Add review section to `tasks/todo. md`  
6. **Capture Lessons**: Update `tasks/lessons. md` after corrections  

## Core Principles

- **Simplicity First**: Make every change as simple as possible. Impact minimal code.
- **No Laziness**: Find root causes. No temporary fixes. Senior developer standards.
