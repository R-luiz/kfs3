#!/bin/bash

set -e

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
sudo tee mnt/boot/grub/grub.cfg > /dev/null << EOF
set timeout=0
set default=0

menuentry "KFS-3" {
    multiboot /boot/kernel.bin
}
EOF

# Copy kernel
sudo cp kernel.bin mnt/boot/

# Create embedded config with menu directly
cat > /tmp/grub_early.cfg << 'EOFCFG'
set root=(hd0,msdos1)
set timeout=0
multiboot /boot/kernel.bin
boot
EOFCFG

# Build core.img with all needed modules embedded
grub-mkimage -O i386-pc \
    -o /tmp/core.img \
    -c /tmp/grub_early.cfg \
    -p "(hd0,msdos1)/boot/grub" \
    biosdisk part_msdos ext2 multiboot

# Write boot.img to MBR (first 440 bytes, preserving partition table)
sudo dd if=/usr/lib/grub/i386-pc/boot.img of=${LOOP_DEVICE} bs=440 count=1 conv=notrunc

# Write core.img starting at sector 1 (after MBR, before partition)
sudo dd if=/tmp/core.img of=${LOOP_DEVICE} bs=512 seek=1 conv=notrunc

# Cleanup temp files
rm -f /tmp/grub_early.cfg /tmp/core.img

# Cleanup
sync
sudo umount mnt
sudo losetup -d ${LOOP_DEVICE}
rm -rf mnt

echo "Image created successfully"
