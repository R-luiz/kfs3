#!/bin/bash

# Create a disk image (10MB as specified in the subject)
dd if=/dev/zero of=os.img bs=1M count=10

# Create partition table
sudo parted -s os.img mklabel msdos
sudo parted -s os.img mkpart primary 1 10
sudo parted -s os.img set 1 boot on

# Set up loopback device
LOOP_DEVICE=$(sudo losetup -Pf --show os.img)

# Format the partition
sudo mkfs.ext2 ${LOOP_DEVICE}p1

# Create mount point and mount
mkdir -p mnt
sudo mount ${LOOP_DEVICE}p1 mnt

# Create directory structure
sudo mkdir -p mnt/boot/grub

# Create GRUB config
cat > grub.cfg << EOF
default=0
timeout=0

menuentry "KFS-1" {
    multiboot /boot/kernel.bin
    boot
}
EOF

sudo mv grub.cfg mnt/boot/grub/

# Copy kernel
sudo cp kernel.bin mnt/boot/

# Install GRUB
sudo grub-install --target=i386-pc --boot-directory=mnt/boot --no-floppy ${LOOP_DEVICE}

# Cleanup
sync
sudo umount mnt
sudo losetup -d ${LOOP_DEVICE}
rm -rf mnt

echo "Image created successfully"