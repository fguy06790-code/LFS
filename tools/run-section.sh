#!/bin/bash
# Runs one LFS book section: unpack its package (if it has one), run the
# section's commands from inside the package directory, then remove it.
#
# Usage: run-section.sh NNN
# Environment: BUILD_DIR (manifest.tsv and run/), SRC_DIR (source tarballs).
# Called by build-lfs.sh as user lfs, as root on the host, or inside the chroot.
set -e
set +h
umask 022
: "${BUILD_DIR:?BUILD_DIR not set}" "${SRC_DIR:?SRC_DIR not set}"

num=${1:?usage: run-section.sh NNN}
line=$(awk -F'\t' -v n="$num" '$1 == n' "$BUILD_DIR/manifest.tsv")
[ -n "$line" ] || { echo "run-section: no section $num in the manifest" >&2; exit 1; }
IFS=$'\t' read -r _ _ id tarball _ title <<< "$line"
script=$BUILD_DIR/run/$num-$id.sh
[ -f "$script" ] || { echo "run-section: missing $script" >&2; exit 1; }

echo "=== $title"
cd "$SRC_DIR"
if [ "$tarball" = "-" ]; then
    exec bash +h -e "$script"
fi

dir=$(tar -tf "$tarball" | sed -n '1{s|^\./||; s|/.*||; p}')
[ -n "$dir" ] || { echo "run-section: cannot find the top directory of $tarball" >&2; exit 1; }
rm -rf "$dir"
tar -xf "$tarball"
cd "$dir"
bash +h -e "$script"
cd "$SRC_DIR"
rm -rf "$dir"
