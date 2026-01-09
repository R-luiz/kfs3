# Compiler settings
CC = gcc
ASM = nasm
LD = ld

# Flags
CFLAGS = -m32 -fno-builtin -fno-exceptions -fno-stack-protector \
         -nostdlib -nodefaultlibs -ffreestanding -O2 -Wall -Wextra \
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
	qemu-system-i386 -hda os.img -m 128M
