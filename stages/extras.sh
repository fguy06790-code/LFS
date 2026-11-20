#!/bin/bash
# Final stage, run inside the chroot after chapter 10:
#   - pe-loader and pe-inspect from lfs-pe-inspect, plus the binfmt_misc boot
#     script so ./program.exe runs directly
#   - Node.js (optional, WITH_NODE=1) so pe-inspect can run
#   - release files from LFS chapter 11
set -e
. /sources/lfs-build/config.env

echo "=== lfs-pe-inspect"
cd /sources/lfs-pe-inspect
make clean
make
make install PREFIX=/usr
make install-binfmt
pe-loader --version

if [ "$WITH_NODE" = 1 ]; then
    echo "=== Node.js $NODE_VERSION"
    cd /sources
    rm -rf "node-v$NODE_VERSION"
    tar -xf "node-v$NODE_VERSION.tar.xz"
    cd "node-v$NODE_VERSION"
    ./configure --prefix=/usr
    make
    make install
    cd /sources
    rm -rf "node-v$NODE_VERSION"
    node --version
fi

echo "=== release files"
echo "$LFS_VERSION" > /etc/lfs-release
cat > /etc/lsb-release << EOF
DISTRIB_ID="Linux From Scratch"
DISTRIB_RELEASE="$LFS_VERSION"
DISTRIB_CODENAME="lfs-pe"
DISTRIB_DESCRIPTION="Linux From Scratch with Windows program support"
EOF
cat > /etc/os-release << EOF
NAME="Linux From Scratch"
VERSION="$LFS_VERSION"
ID=lfs
PRETTY_NAME="Linux From Scratch $LFS_VERSION"
VERSION_CODENAME="lfs-pe"
HOME_URL="https://www.linuxfromscratch.org/lfs/"
RELEASE_TYPE="stable"
EOF
echo "extras done"
