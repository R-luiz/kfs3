# Compiler settings
CC = gcc
ASM = nasm
LD = ld

# Flags
CFLAGS = -m32 -fno-builtin -fno-exceptions -fno-stack-protector \
         -nostdlib -nodefaultlibs -ffreestanding -O2 -Wall -Wextra \
         -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
         -I kernel/include

ASMFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T boot/linker.ld -nostdlib

# Source files
KERNEL_OBJS = boot/boot.o \
              kernel/src/main.o \
              kernel/src/keyboard.o \
              kernel/src/gdt.o \
              kernel/src/shell.o \
              kernel/src/panic.o \
              kernel/src/pmm.o \
              kernel/src/paging.o \
              kernel/src/kmalloc.o \
              kernel/src/vmalloc.o

# Targets
.PHONY: all clean run image

all: kernel.bin

kernel.bin: $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	$(ASM) $(ASMFLAGS) $< -o $@

image: kernel.bin
	@chmod +x tools/create_image.sh
	@./tools/create_image.sh

clean:
	rm -f $(KERNEL_OBJS) kernel.bin os.img

run: kernel.bin
	qemu-system-i386 -kernel kernel.bin -m 128M -display gtk

run-image: image
	qemu-system-i386 -drive format=raw,file=os.img -m 128M

# Debug targets
debug: kernel.bin
	qemu-system-i386 -kernel kernel.bin -m 128M -d int,cpu_reset -no-reboot -no-shutdown

debug-image: image
	qemu-system-i386 -drive format=raw,file=os.img -m 128M -d int,cpu_reset -no-reboot -no-shutdown 2>&1 | head -100

# Check multiboot header
check-multiboot:
	@echo "Checking multiboot header in kernel.bin..."
	@xxd kernel.bin | head -20
	@echo ""
	@echo "Looking for multiboot magic (0x1BADB002)..."
	@hexdump -C kernel.bin | grep -i "02 b0 ad 1b" || echo "Magic not found in expected location"
