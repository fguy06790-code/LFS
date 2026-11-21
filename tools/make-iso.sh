#!/bin/bash
# Make a bootable live ISO from a finished LFS disk image.
#
# Usage (as root, on the build host):  tools/make-iso.sh LFS.IMG OUT.iso
#
# The whole system (minus /sources and /usr/share/doc) is packed into the
# initramfs, so the kernel unpacks it straight into RAM and runs /sbin/init
# from there: no extra drivers or a separate startup stage needed, and nothing
# on the machine's disks is touched. Changes are lost at reboot.
#
# The ISO boots on BIOS and UEFI (Secure Boot off) and can be written to a USB
# stick (Rufus in "DD image" mode, balenaEtcher, or dd). It needs about 4 GB RAM.
#
# Host tools: grub-mkrescue (grub-pc-bin + grub-efi-amd64-bin), xorriso,
# mtools, cpio, zstd.
set -euo pipefail

[ $# -eq 2 ] || { sed -n '2,15p' "$0"; exit 1; }
IMG=$(realpath "$1") OUT=$(realpath -m "$2")
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
for t in grub-mkrescue xorriso mformat cpio zstd; do
    command -v "$t" >/dev/null || { echo "missing host tool: $t"; exit 1; }
done

WORK=$(mktemp -d /var/tmp/lfs-iso.XXXXXX)
LOOP=
cleanup() {
    mountpoint -q "$WORK/mnt" && umount "$WORK/mnt"
    [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT
mkdir -p "$WORK"/{mnt,root,iso/boot/grub}

echo "==> Reading $IMG"
LOOP=$(losetup -fP -r --show "$IMG")
mount -o ro "${LOOP}p1" "$WORK/mnt"
M=$WORK/mnt R=$WORK/root

KERNEL=$(ls "$M"/boot/vmlinuz-* | head -1)
cp "$KERNEL" "$WORK/iso/boot/vmlinuz"
VERSION=$(. "$M/etc/os-release" 2>/dev/null; echo "${VERSION_ID:-12.4}")

echo "==> Copying the system (without /sources and docs)"
tar -C "$M" --numeric-owner --xattrs \
    --exclude=./sources --exclude=./usr/share/doc --exclude=./lost+found \
    --exclude='./boot/*' --exclude='./tmp/*' --exclude='./var/tmp/*' \
    -cf - . | tar -C "$R" --numeric-owner --xattrs -xpf -
umount "$M"; losetup -d "$LOOP"; LOOP=

echo "==> Live-system changes"
# The kernel runs /init from the initramfs; make that SysVinit.
ln -sf sbin/init "$R/init"
# init needs a console before devtmpfs is mounted.
mknod -m 600 "$R/dev/console" c 5 1
mknod -m 666 "$R/dev/null" c 1 3
# No root partition or swap: keep only the virtual file systems.
grep -vE '^[[:space:]]*[^#[:space:]]+[[:space:]]+(/|swap)[[:space:]]' "$R/etc/fstab" > "$R/etc/fstab.live"
mv "$R/etc/fstab.live" "$R/etc/fstab"
# Root lives in RAM (tmpfs): there is nothing to fsck or remount.
rm -f "$R"/etc/rc.d/rcS.d/S[0-9][0-9]checkfs
sed -i -E 's@^([[:space:]]*)(mount (-n )?(--options|-o) remount,(ro|rw) /.*)$@\1\2 || true@' \
    "$R/etc/rc.d/init.d/mountfs"
printf '\nLinux From Scratch %s (live, running from RAM; changes are lost at reboot)\n\n' \
    "$VERSION" >> "$R/etc/issue"

echo "==> Packing the initramfs (zstd)"
(cd "$R" && find . -print0 | cpio --null -o -H newc --quiet) \
    | zstd -q -T0 -15 > "$WORK/iso/boot/initrd.zst"
ls -lh "$WORK/iso/boot/initrd.zst" | awk '{print "    initramfs: " $5}'
rm -rf "$R"

cat > "$WORK/iso/boot/grub/grub.cfg" <<EOF
set timeout=5
set default=0
insmod all_video
insmod gfxterm
terminal_output gfxterm

menuentry "Linux From Scratch $VERSION (live)" {
    echo "Loading the system into RAM, this takes a moment..."
    linux /boot/vmlinuz net.ifnames=0 console=ttyS0,115200 console=tty0
    initrd /boot/initrd.zst
}
menuentry "Linux From Scratch $VERSION (live, basic graphics)" {
    echo "Loading the system into RAM, this takes a moment..."
    linux /boot/vmlinuz net.ifnames=0 nomodeset console=ttyS0,115200 console=tty0
    initrd /boot/initrd.zst
}
EOF

echo "==> Building $OUT"
grub-mkrescue -o "$OUT" "$WORK/iso" -- -volid LFS_LIVE 2>&1 | grep -vE '^xorriso|^Drive current|^Media current|^Media status|^Media summary|^Added to ISO|^Written to medium|^Writing to|^libisofs|^$|UPDATE' || true
[ -s "$OUT" ] || { echo "grub-mkrescue failed"; exit 1; }
ls -lh "$OUT" | awk '{print "    " $9 ": " $5}'
