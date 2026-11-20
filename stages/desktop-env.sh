#!/bin/bash
# Runs once inside the chroot before the BLFS desktop build.
# The BLFS book lets you install Xorg and KDE either into /usr or into
# /opt/xorg and /opt/kf6. This build uses /usr for both, which needs no extra
# search paths (see "Setting up the Xorg Build Environment" and "KDE Frameworks 6
# Pre-installation Configuration" in BLFS).
set -e
install -d -m 755 /etc/profile.d

cat > /etc/profile.d/xorg.sh << "EOF"
XORG_PREFIX="/usr"
XORG_CONFIG="--prefix=$XORG_PREFIX --sysconfdir=/etc --localstatedir=/var --disable-static"
export XORG_PREFIX XORG_CONFIG
EOF
chmod 644 /etc/profile.d/xorg.sh

cat > /etc/profile.d/kf6.sh << "EOF"
# Begin /etc/profile.d/kf6.sh

export KF6_PREFIX=/usr

# End /etc/profile.d/kf6.sh
EOF
chmod 644 /etc/profile.d/kf6.sh
echo "desktop environment settings written"
