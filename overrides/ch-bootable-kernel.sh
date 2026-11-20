# 10.3. Linux kernel (override), run from inside the kernel source directory.
# The book uses "make menuconfig", which is interactive. This starts from the
# x86_64 defconfig and turns on:
#   - the options the LFS book requires (devtmpfs, PSI, memcg, framebuffer console, ...)
#   - disk, network, and console drivers for QEMU/KVM and Hyper-V, built in so
#     no initramfs is needed
#   - BINFMT_MISC, so the kernel can hand .exe files to pe-loader
. /sources/lfs-build/config.env

make mrproper
make defconfig
scripts/config \
    -d WERROR \
    -e PSI -e MEMCG -e RELOCATABLE -e RANDOMIZE_BASE -e STACKPROTECTOR_STRONG \
    -e DEVTMPFS -e DEVTMPFS_MOUNT \
    -e DRM -e DRM_FBDEV_EMULATION -e DRM_SIMPLEDRM -e FRAMEBUFFER_CONSOLE \
    -e BINFMT_MISC \
    -e EXT4_FS \
    -e ATA -e ATA_PIIX -e SATA_AHCI \
    -e VIRTIO -e VIRTIO_PCI -e VIRTIO_BLK -e VIRTIO_NET -e SCSI_VIRTIO \
    -e E1000 -e E1000E \
    -e HYPERVISOR_GUEST -e HYPERV -e HYPERV_STORAGE -e HYPERV_NET \
    -e HYPERV_KEYBOARD -e HID_HYPERV_MOUSE \
    -e SERIAL_8250 -e SERIAL_8250_CONSOLE \
    -e IKCONFIG -e IKCONFIG_PROC

# Desktop (KDE Plasma): VM graphics, input, sound, and what elogind,
# PipeWire, and Flatpak-style sandboxes (bubblewrap) expect from the kernel.
if [ "${DESKTOP:-none}" != none ]; then
    scripts/config \
        -e DRM_VIRTIO_GPU -e DRM_BOCHS -e DRM_QXL -e DRM_VMWGFX -e DRM_HYPERV \
        -e INPUT_EVDEV -e INPUT_UINPUT -e USB_HID -e HID_GENERIC \
        -e USB_XHCI_HCD -e USB_EHCI_HCD \
        -e SOUND -e SND -e SND_HDA_INTEL -e SND_HDA_GENERIC \
        -e CGROUPS -e INOTIFY_USER -e TMPFS_POSIX_ACL -e FUSE_FS \
        -e USER_NS -e SECCOMP -e SECCOMP_FILTER
fi
make olddefconfig

# Check the options that matter for booting and for pe-loader really ended up on.
for opt in DEVTMPFS DEVTMPFS_MOUNT EXT4_FS BINFMT_MISC ATA_PIIX VIRTIO_BLK; do
    grep -q "^CONFIG_$opt=y" .config || { echo "kernel option $opt is not built in" >&2; exit 1; }
done

make
make modules_install

KVER=$(make -s kernelrelease)
cp -v arch/x86/boot/bzImage "/boot/vmlinuz-$KVER-lfs-$LFS_VERSION"
cp -v System.map "/boot/System.map-$KVER"
cp -v .config "/boot/config-$KVER"

install -v -m755 -d /etc/modprobe.d
cat > /etc/modprobe.d/usb.conf << "EOF"
# Begin /etc/modprobe.d/usb.conf

install ohci_hcd /sbin/modprobe ehci_hcd ; /sbin/modprobe -i ohci_hcd ; true
install uhci_hcd /sbin/modprobe ehci_hcd ; /sbin/modprobe -i uhci_hcd ; true

# End /etc/modprobe.d/usb.conf
EOF
