# Compiler settings
CC = gcc
ASM = nasm
LD = ld
PYTHON ?= python3
QEMU = qemu-system-i386

QEMU_MEMORY = 128M
QEMU_DISPLAY_BACKEND = $(shell $(QEMU) -display help 2>/dev/null | sed -n 's/^[[:space:]]*\([[:alnum:]_-][[:alnum:]_-]*\)$$/\1/p' | grep -v '^none$$' | head -n 1)

ifeq ($(strip $(QEMU_DISPLAY_BACKEND)),)
QEMU_DISPLAY_FLAGS = -nographic -serial stdio -monitor none
else
QEMU_DISPLAY_FLAGS = -display $(QEMU_DISPLAY_BACKEND)
endif

# Build directory (default: in-tree; use BUILD_DIR=build for out-of-tree)
BUILD_DIR ?= .

# Flags
CFLAGS = -m32 -fno-builtin -fno-exceptions -fno-stack-protector \
         -nostdlib -nodefaultlibs -ffreestanding -O2 -Wall -Wextra \
         -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
         -I kernel/include

ASMFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T boot/linker.ld -nostdlib

# Source files (prefixed with BUILD_DIR for out-of-tree builds)
KERNEL_OBJS = $(BUILD_DIR)/boot/boot.o \
              $(BUILD_DIR)/kernel/src/main.o \
              $(BUILD_DIR)/kernel/src/timer.o \
              $(BUILD_DIR)/kernel/src/process.o \
              $(BUILD_DIR)/kernel/src/process_switch.o \
              $(BUILD_DIR)/kernel/src/interrupts.o \
              $(BUILD_DIR)/kernel/src/idt.o \
              $(BUILD_DIR)/kernel/src/keyboard.o \
              $(BUILD_DIR)/kernel/src/signals.o \
              $(BUILD_DIR)/kernel/src/syscall.o \
              $(BUILD_DIR)/kernel/src/serial.o \
              $(BUILD_DIR)/kernel/src/gdt.o \
              $(BUILD_DIR)/kernel/src/shell.o \
              $(BUILD_DIR)/kernel/src/panic.o \
              $(BUILD_DIR)/kernel/src/pmm.o \
              $(BUILD_DIR)/kernel/src/paging.o \
              $(BUILD_DIR)/kernel/src/kmalloc.o \
              $(BUILD_DIR)/kernel/src/vmalloc.o

# Test filtering (e.g., make test-kfs KFS=3, or make test-kfs KFS="2 3")
KFS ?=

# Targets
.PHONY: all clean run image test-kfs

all: $(BUILD_DIR)/kernel.bin

$(BUILD_DIR)/kernel.bin: $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASMFLAGS) $< -o $@

image: $(BUILD_DIR)/kernel.bin
	@chmod +x tools/create_image.sh
	@./tools/create_image.sh

clean:
	rm -f $(KERNEL_OBJS) $(BUILD_DIR)/kernel.bin os.img
	@if [ "$(BUILD_DIR)" != "." ]; then rm -rf $(BUILD_DIR); fi

run: $(BUILD_DIR)/kernel.bin
	$(QEMU) -kernel $(BUILD_DIR)/kernel.bin -m $(QEMU_MEMORY) $(QEMU_DISPLAY_FLAGS)

test-kfs: $(BUILD_DIR)/kernel.bin
	$(PYTHON) kfs-testing/run.py --skip-build $(if $(KFS),--kfs $(KFS))

run-image: image
	$(QEMU) -drive format=raw,file=os.img -m $(QEMU_MEMORY) $(QEMU_DISPLAY_FLAGS)

# Debug targets
debug: $(BUILD_DIR)/kernel.bin
	$(QEMU) -kernel $(BUILD_DIR)/kernel.bin -m $(QEMU_MEMORY) $(QEMU_DISPLAY_FLAGS) -d int,cpu_reset -no-reboot -no-shutdown

debug-image: image
	$(QEMU) -drive format=raw,file=os.img -m $(QEMU_MEMORY) $(QEMU_DISPLAY_FLAGS) -d int,cpu_reset -no-reboot -no-shutdown 2>&1 | head -100

# Check multiboot header
check-multiboot:
	@echo "Checking multiboot header in $(BUILD_DIR)/kernel.bin..."
	@xxd $(BUILD_DIR)/kernel.bin | head -20
	@echo ""
	@echo "Looking for multiboot magic (0x1BADB002)..."
	@hexdump -C $(BUILD_DIR)/kernel.bin | grep -i "02 b0 ad 1b" || echo "Magic not found in expected location"
