#!/bin/bash
# Runs one BLFS section inside the chroot.
#
# Usage: run-blfs-section.sh NNN
# Environment: BLFS_DIR (normally /sources/blfs) with plan/, cache/, bootscripts/.
#
# Everything the section needs was downloaded on the host into cache/<id>/.
# Here, a wget stand-in copies those files instead of going online, so the
# book's own commands (including bulk pages' download steps) run unchanged.
set -e
set +h
umask 022
: "${BLFS_DIR:?BLFS_DIR not set}"

num=${1:?usage: run-blfs-section.sh NNN}
line=$(awk -F'\t' -v n="$num" '$1 == n' "$BLFS_DIR/plan/plan.tsv")
[ -n "$line" ] || { echo "run-blfs-section: no section $num in the plan" >&2; exit 1; }
IFS=$'\t' read -r _ id url _ title <<< "$line"
script=$BLFS_DIR/plan/$num-$id.sh
export BLFS_CACHE=$BLFS_DIR/cache/$id
export BLFS_BOOTSCRIPTS=$BLFS_DIR/bootscripts
work=$BLFS_DIR/work/$id

# BLFS keeps its settings in /etc/profile and /etc/profile.d
# (XORG_PREFIX, KF6_PREFIX, QT6DIR, PKG_CONFIG_PATH, ...). Those scripts are
# not written for "set -e", so load them with it off.
keep_makeflags=$MAKEFLAGS
set +e
# shellcheck source=/dev/null
source /etc/profile >/dev/null 2>&1
set -e
export MAKEFLAGS=$keep_makeflags

wget() {
    if [ -d "$BLFS_CACHE" ]; then
        cp -n "$BLFS_CACHE"/* . 2>/dev/null || true
    fi
}
export -f wget

echo "=== $title"
rm -rf "$work"
mkdir -p "$work"
cd "$work"
[ -d "$BLFS_CACHE" ] && cp -a "$BLFS_CACHE"/. .

if [ "$url" != "-" ]; then
    tb=${url##*/}
    case "$tb" in
        *.zip) dir=$(unzip -Z1 "$tb" | sed -n '1{s|/.*||; p}'); unzip -q "$tb" ;;
        *)     dir=$(tar -tf "$tb" | sed -n '1{s|^\./||; s|/.*||; p}'); tar -xf "$tb" ;;
    esac
    [ -n "$dir" ] && [ -d "$dir" ] || { echo "run-blfs-section: cannot find the source directory in $tb" >&2; exit 1; }
    cd "$dir"
fi

bash +h -e "$script"

cd /
rm -rf "$work"
/sbin/ldconfig
