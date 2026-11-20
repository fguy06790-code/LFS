# 10.2. Creating the /etc/fstab File (override)
# The book's device names are placeholders. The root partition is identified
# by its PARTUUID, so it works whatever the VM calls the disk (sda, vda, ...).
. /sources/lfs-build/disk.env

cat > /etc/fstab << EOF
# Begin /etc/fstab

# file system        mount-point    type     options             dump  fsck
#                                                                      order

PARTUUID=$PARTUUID   /              ext4     defaults            1     1
proc                 /proc          proc     nosuid,noexec,nodev 0     0
sysfs                /sys           sysfs    nosuid,noexec,nodev 0     0
devpts               /dev/pts       devpts   gid=5,mode=620      0     0
tmpfs                /run           tmpfs    defaults            0     0
devtmpfs             /dev           devtmpfs mode=0755,nosuid    0     0
tmpfs                /dev/shm       tmpfs    nosuid,nodev        0     0
cgroup2              /sys/fs/cgroup cgroup2  nosuid,noexec,nodev 0     0

# End /etc/fstab
EOF
