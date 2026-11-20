# 10.4. Using GRUB to Set Up the Boot Process (override)
# The book installs to /dev/sda and makes an optional rescue CD. Here GRUB goes
# onto the disk image (through its loop device), which the VM sees as its
# first disk. The kernel finds the root partition by PARTUUID.
. /sources/lfs-build/config.env
. /sources/lfs-build/disk.env

grub-install --target=i386-pc --boot-directory=/boot "$LOOP_DEV"

KIMG=$(cd /boot && ls vmlinuz-*-lfs-* | sort | tail -1)
[ -n "$KIMG" ] || { echo "no kernel found in /boot" >&2; exit 1; }

cat > /boot/grub/grub.cfg << EOF
# Begin /boot/grub/grub.cfg
set default=0
set timeout=5

insmod part_msdos
insmod ext2
search --no-floppy --fs-uuid --set=root $FS_UUID

menuentry "GNU/Linux, LFS $LFS_VERSION ($KIMG)" {
        linux   /boot/$KIMG root=PARTUUID=$PARTUUID ro net.ifnames=0 console=tty0 console=ttyS0,115200
}
EOF
