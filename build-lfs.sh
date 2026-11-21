#!/bin/bash
# build-lfs.sh: build Linux From Scratch (SysVinit edition) into a bootable
# disk image, with Windows program support from lfs-pe-inspect
# (https://github.com/fguy06790-code/lfs-pe-inspect) installed at the end.
#
# Run as root on a Linux host. WSL2 works. The script is resumable: if a step
# fails, fix the cause and run it again; finished steps are skipped.
#
#   ./build-lfs.sh                 build everything
#   ./build-lfs.sh --fix-host      install missing host tools first (Debian/Ubuntu)
#   ./build-lfs.sh --stop-after disk
#   ./build-lfs.sh --config my.conf
#
# Phases, in order:
#   preflight  check the host (and ask for the new system's root password)
#   package    download lfs-pe-inspect from GitHub and build it on the host
#   book       download the LFS book and turn its commands into scripts
#   disk       create the disk image, partition it, and mount it
#   sources    download every LFS source package and check its MD5 sum
#   layout     chapter 4: directory layout and the unprivileged lfs user
#   cross      chapter 5: the cross toolchain (as user lfs)
#   temptools  chapter 6: temporary tools (as user lfs)
#   chroot     chapter 7: enter the chroot and finish the temporary tools
#   system     chapter 8: the final system
#   config     chapter 9: boot scripts and system configuration
#   boot       chapter 10: fstab, kernel, and GRUB
#   extras     pe-loader, pe-inspect, binfmt_misc boot script, release files
#   desktop    KDE Plasma and the SDDM login screen from BLFS (DESKTOP=plasma)
#   finish     unmount and print how to boot the image
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PHASES=(preflight package book disk sources layout cross temptools chroot system config boot extras desktop finish)

# Sections the script handles itself, or that are only examples in the book.
SKIP_SECTIONS=" ch-tools-kernfs ch-tools-chroot ch-system-pkgmgt ch-config-symlinks "
# Sections that run as root on the host instead of in the chroot.
HOST_SECTIONS=" ch-tools-changingowner "
# Sections with placeholders that edit_section() fills in.
EDITED_SECTIONS=" ch-system-glibc ch-system-groff ch-system-shadow "

FIX_HOST=0
STOP_AFTER=""
CONFIG=""
ATTACHED=0

# ---- Output -------------------------------------------------------------------
log()  { printf '\n==> %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }
die()  { printf '\nbuild-lfs: error: %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '2,/^set -euo/{/^set -euo/d; s/^# \{0,1\}//; p}' "$0"
    exit "${1:-0}"
}

# ---- Arguments and configuration --------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --fix-host)   FIX_HOST=1 ;;
        --stop-after) STOP_AFTER=${2:?--stop-after needs a phase}; shift ;;
        --config)     CONFIG=${2:?--config needs a file}; shift ;;
        --keep-tests) KEEP_TESTS=1 ;;
        --with-node)  WITH_NODE=1 ;;
        -h|--help)    usage 0 ;;
        *)            printf 'unknown option: %s\n\n' "$1" >&2; usage 2 ;;
    esac
    shift
done

if [ -z "$CONFIG" ] && [ -f "$SCRIPT_DIR/lfs.conf" ]; then
    CONFIG=$SCRIPT_DIR/lfs.conf
fi
if [ -n "$CONFIG" ]; then
    [ -f "$CONFIG" ] || die "config file not found: $CONFIG"
    # shellcheck source=/dev/null
    . "$CONFIG"
fi

if [ -n "$STOP_AFTER" ] && [[ " ${PHASES[*]} " != *" $STOP_AFTER "* ]]; then
    die "unknown phase for --stop-after: $STOP_AFTER (phases: ${PHASES[*]})"
fi

LFS_VERSION=${LFS_VERSION:-12.4}
DESKTOP=${DESKTOP:-plasma}           # plasma, or none for a console-only system
WORK=${WORK:-/root/lfs-work}
LFS=${LFS:-/mnt/lfs}
IMAGE=${IMAGE:-$WORK/lfs.img}
if [ "$DESKTOP" = none ]; then IMAGE_SIZE=${IMAGE_SIZE:-40G}; else IMAGE_SIZE=${IMAGE_SIZE:-120G}; fi
JOBS=${JOBS:-$(nproc)}
PKG_REPO=${PKG_REPO:-https://github.com/fguy06790-code/lfs-pe-inspect.git}
PKG_REF=${PKG_REF:-main}
TIMEZONE=${TIMEZONE:-UTC}
PAPER=${PAPER:-letter}
LFS_LANG=${LFS_LANG:-en_US.UTF-8}
HOST_NAME=${HOST_NAME:-lfs}
NET_IP=${NET_IP:-10.0.2.15}
NET_PREFIX=${NET_PREFIX:-24}
NET_GATEWAY=${NET_GATEWAY:-10.0.2.2}
NET_BROADCAST=${NET_BROADCAST:-10.0.2.255}
NET_DNS=${NET_DNS:-10.0.2.3}
WITH_NODE=${WITH_NODE:-0}
NODE_VERSION=${NODE_VERSION:-22.18.0}
KEEP_TESTS=${KEEP_TESTS:-0}
VERBOSE=${VERBOSE:-0}

# BLFS desktop build: the targets, plus recommended packages a usable desktop
# needs (LLVM gives Mesa its software renderer for VMs without a real GPU).
BLFS_TARGETS=${BLFS_TARGETS:-"plasma-build sddm"}
BLFS_WITH=${BLFS_WITH:-postlfs-config-profile,make-ca,linux-pam,elogind,polkit,llvm,libinput,breeze-icons,xwayland}
BLFS_URL=https://www.linuxfromscratch.org/blfs/downloads/$LFS_VERSION/BLFS-BOOK-$LFS_VERSION-nochunks.html.xz

BOOK_BASE=https://www.linuxfromscratch.org/lfs
BOOK_URL=$BOOK_BASE/downloads/$LFS_VERSION/LFS-BOOK-$LFS_VERSION-NOCHUNKS.html
LIST_URL=$BOOK_BASE/view/$LFS_VERSION/wget-list-sysv
MD5_URL=$BOOK_BASE/view/$LFS_VERSION/md5sums
LFS_MIRROR=${LFS_MIRROR:-https://ftp.osuosl.org/pub/lfs/lfs-packages/$LFS_VERSION}

BUILD=$WORK/build                    # composed scripts, copied into the image
ROOT_HASH=$WORK/.root-password-hash  # hashed root password, until chapter 8 uses it
IN_IMAGE_BUILD=/sources/lfs-build    # the same directory, seen from the chroot

# ---- State --------------------------------------------------------------------------
mark_done() { touch "$WORK/state/$1"; }
is_done()   { [ -e "$WORK/state/$1" ]; }
in_list()   { [[ "$2" == *" $1 "* ]]; }

# ---- Host check ---------------------------------------------------------------------
host_problems() {
    local p=() c
    for c in bash ld bison gcc g++ make patch perl python3 sed tar makeinfo xz gzip \
             m4 gawk find diff wget git sfdisk losetup mkfs.ext4 blkid openssl xz; do
        command -v "$c" >/dev/null 2>&1 || p+=("missing command: $c")
    done
    [[ "$(readlink -f /bin/sh)" == */bash ]] || p+=("/bin/sh is not bash (LFS requires it)")
    awk --version 2>/dev/null | head -1 | grep -q GNU || p+=("awk is not GNU awk")
    yacc --version 2>/dev/null | head -1 | grep -qi bison || p+=("yacc is not bison")
    if command -v g++ >/dev/null 2>&1; then
        echo 'int main(){}' | g++ -x c++ - -o "$WORK/.gxx-test" 2>/dev/null || p+=("g++ cannot compile")
        rm -f "$WORK/.gxx-test"
    fi
    [ -e /dev/loop-control ] || p+=("no loop device support (/dev/loop-control missing)")
    local avail
    local need=35
    [ "$DESKTOP" = none ] || need=110
    avail=$(df --output=avail -BG "$(dirname "$IMAGE")" | tail -1 | tr -dc 0-9)
    [ "${avail:-0}" -ge "$need" ] || p+=("less than $need GB free for the image (${avail:-0} GB)")
    if [ ${#p[@]} -gt 0 ]; then printf '%s\n' "${p[@]}"; fi
}

fix_host() {
    command -v apt-get >/dev/null || die "--fix-host only supports apt (Debian/Ubuntu); install the tools by hand"
    log "Installing host tools with apt"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq build-essential bison gawk m4 texinfo python3 wget git \
        xz-utils patch perl fdisk e2fsprogs openssl >/dev/null
    ln -sf bash /bin/sh
    update-alternatives --set awk /usr/bin/gawk >/dev/null 2>&1 || true
    if ! yacc --version 2>/dev/null | head -1 | grep -qi bison; then
        update-alternatives --set yacc /usr/bin/bison.yacc >/dev/null 2>&1 || true
    fi
}

phase_preflight() {
    log "Checking the host"
    [ "$(id -u)" = 0 ] || die "run as root (the build needs loop devices, mounts, and chroot)"
    mkdir -p "$WORK/state" "$WORK/logs" "$WORK/book" "$(dirname "$IMAGE")"
    local problems
    problems=$(host_problems)
    if [ -n "$problems" ] && [ "$FIX_HOST" = 1 ]; then
        fix_host
        problems=$(host_problems)
    fi
    if [ -n "$problems" ]; then
        printf '%s\n' "$problems" | sed 's/^/    - /' >&2
        die "the host is not ready. Fix the items above, or rerun with --fix-host on Debian/Ubuntu."
    fi
    info "host OK: $(uname -r), $JOBS parallel jobs, work dir $WORK"

    # The new system's root password is asked once and kept only as a SHA-512
    # crypt hash in a root-only file, so a resumed build doesn't ask again.
    # The file is deleted once chapter 8 has set the password.
    if ! is_done phase-system && [ ! -s "$ROOT_HASH" ]; then
        local a=${LFS_ROOT_PASSWORD:-} b
        if [ -z "$a" ]; then
            [ -t 0 ] || die "set LFS_ROOT_PASSWORD, or run in a terminal so the script can ask for it"
            read -rsp "    Root password for the new LFS system: " a; echo
            read -rsp "    Repeat it: " b; echo
            [ "$a" = "$b" ] || die "the passwords don't match"
        fi
        [ -n "$a" ] || die "the root password can't be empty"
        (umask 077; printf '%s' "$a" | openssl passwd -6 -stdin > "$ROOT_HASH")
        [ -s "$ROOT_HASH" ] || die "could not hash the root password (is openssl installed?)"
        info "root password saved as a hash for this build"
    fi
}

# ---- Package: lfs-pe-inspect from GitHub -------------------------------------------
phase_package() {
    log "Downloading the Windows-support package from $PKG_REPO"
    local dir=$WORK/lfs-pe-inspect
    if [ -d "$dir/.git" ]; then
        git -C "$dir" fetch -q origin "$PKG_REF"
        git -C "$dir" checkout -q FETCH_HEAD
    else
        rm -rf "$dir"
        git clone -q --branch "$PKG_REF" "$PKG_REPO" "$dir"
    fi
    info "at commit $(git -C "$dir" rev-parse --short HEAD)"
    log "Building it on the host to make sure it works before the long build"
    make -s -C "$dir" clean >/dev/null
    make -s -C "$dir" >/dev/null
    if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
        make -s -C "$dir" check | tail -1
    else
        info "x86_64-w64-mingw32-gcc not installed: skipping its tests (the build is still checked)"
    fi
    make -s -C "$dir" clean >/dev/null
}

# ---- Book: download and extract commands -------------------------------------------
phase_book() {
    log "Downloading the LFS $LFS_VERSION book, source list, and checksums"
    wget -q -O "$WORK/book/lfs.html" "$BOOK_URL"
    wget -q -O "$WORK/book/wget-list-sysv" "$LIST_URL"
    wget -q -O "$WORK/book/md5sums" "$MD5_URL"
    log "Turning the book's commands into build scripts"
    local args=()
    [ "$KEEP_TESTS" = 1 ] && args+=(--keep-tests)
    rm -rf "$WORK/cmds"
    python3 "$SCRIPT_DIR/tools/extract-book.py" "$WORK/book/lfs.html" \
        "$WORK/book/wget-list-sysv" "$WORK/cmds" "${args[@]}" | sed 's/^/    /'
}

# Fill in the book's placeholders for the few sections that have them.
edit_section() {
    local id=$1 f=$2
    case "$id" in
        ch-system-glibc)
            sed -i -e '/^tzselect$/d' \
                   -e "s|/usr/share/zoneinfo/<xxx>|/usr/share/zoneinfo/$TIMEZONE|" "$f"
            ! grep -q '<xxx>' "$f" || die "could not fill in the time zone in $f" ;;
        ch-system-groff)
            sed -i "s|<paper_size>|$PAPER|" "$f"
            ! grep -q '<paper_size>' "$f" || die "could not fill in the paper size in $f" ;;
        ch-system-shadow)
            sed -i "s|^passwd root\$|chpasswd -e < $IN_IMAGE_BUILD/.rootpw \&\& rm -f $IN_IMAGE_BUILD/.rootpw|" "$f"
            grep -q chpasswd "$f" || die "could not replace 'passwd root' in $f" ;;
    esac
}

# Build the final script for each section: an override if we have one, otherwise
# the book's commands with placeholders filled in.
compose_build_dir() {
    local num ch id tarball flags title src dst
    rm -rf "$BUILD"
    mkdir -p "$BUILD/run"
    cp "$WORK/cmds/manifest.tsv" "$BUILD/"
    cp "$SCRIPT_DIR/tools/run-section.sh" "$SCRIPT_DIR/stages/extras.sh" "$BUILD/"
    while IFS=$'\t' read -r num ch id tarball flags title; do
        in_list "$id" "$SKIP_SECTIONS" && continue
        src=$WORK/cmds/$num-$id.sh
        dst=$BUILD/run/$num-$id.sh
        if [ -f "$SCRIPT_DIR/overrides/$id.sh" ]; then
            cp "$SCRIPT_DIR/overrides/$id.sh" "$dst"
        else
            cp "$src" "$dst"
            edit_section "$id" "$dst"
            if [ "$flags" = R ] && ! in_list "$id" "$EDITED_SECTIONS"; then
                die "section $id ($title) has placeholders and no override; add overrides/$id.sh"
            fi
        fi
        # The book starts a fresh login shell here; our runner already has a clean one.
        sed -i '/^exec \/usr\/bin\/bash --login$/d' "$dst"
        # Without the test suites there are no test results to inspect, and a
        # grep that finds nothing returns 1 (e.g. glibc's "grep Timed out $(find ...)").
        if [ "$KEEP_TESTS" != 1 ]; then
            sed -i -E '/^grep .*\$\(find -name /d; /check-log\b/d' "$dst"
            # Test runners that share a block with "make install" (util-linux).
            sed -i -E '/^bash tests\/run\.sh /d' "$dst"
        fi
        # Example commands in the book's notes end in a literal "..." (like GMP's
        # "ABI=32 ./configure ..." for 32-bit hosts); they are not meant to run.
        sed -i -E '/^[^#].* \.\.\.[[:space:]]*$/d' "$dst"
        # Interactive demos (Vim's "vim -c ':options'") need a terminal and a person.
        sed -i -E '/^vim -c /d' "$dst"
    done < "$WORK/cmds/manifest.tsv"

    {
        printf 'LFS_VERSION=%q\nTIMEZONE=%q\nLFS_LANG=%q\nHOST_NAME=%q\n' \
            "$LFS_VERSION" "$TIMEZONE" "$LFS_LANG" "$HOST_NAME"
        printf 'NET_IP=%q\nNET_PREFIX=%q\nNET_GATEWAY=%q\nNET_BROADCAST=%q\nNET_DNS=%q\n' \
            "$NET_IP" "$NET_PREFIX" "$NET_GATEWAY" "$NET_BROADCAST" "$NET_DNS"
        printf 'WITH_NODE=%q\nNODE_VERSION=%q\nDESKTOP=%q\n' "$WITH_NODE" "$NODE_VERSION" "$DESKTOP"
    } > "$BUILD/config.env"
}

# ---- Disk image ------------------------------------------------------------------------
partition_dev() {
    local dev="${1}p1" i
    for i in $(seq 1 20); do
        [ -b "$dev" ] && { echo "$dev"; return; }
        partx -u "$1" 2>/dev/null || true
        sleep 0.25
    done
    die "partition device $dev did not appear"
}

ensure_disk() {
    [ -f "$IMAGE" ] || die "disk image $IMAGE is missing (delete $WORK/state/phase-disk to recreate it)"
    LOOP_DEV=$(losetup -j "$IMAGE" | head -1 | cut -d: -f1)
    if [ -z "$LOOP_DEV" ]; then
        LOOP_DEV=$(losetup -fP --show "$IMAGE")
        ATTACHED=1
    fi
    PART_DEV=$(partition_dev "$LOOP_DEV")
    mkdir -p "$LFS"
    if ! mountpoint -q "$LFS"; then
        mount -o noatime "$PART_DEV" "$LFS"
        ATTACHED=1
    fi
    {
        printf 'LOOP_DEV=%q\nPART_DEV=%q\n' "$LOOP_DEV" "$PART_DEV"
        printf 'PARTUUID=%q\nFS_UUID=%q\n' "$(blkid -s PARTUUID -o value "$PART_DEV")" \
            "$(blkid -s UUID -o value "$PART_DEV")"
    } > "$BUILD/disk.env"
    sync_build_dir
}

sync_build_dir() {
    [ -d "$LFS/sources" ] || return 0
    rm -rf "$LFS$IN_IMAGE_BUILD"
    cp -a "$BUILD" "$LFS$IN_IMAGE_BUILD"
}

phase_disk() {
    log "Creating a $IMAGE_SIZE disk image at $IMAGE"
    local old
    old=$(losetup -j "$IMAGE" 2>/dev/null | cut -d: -f1 || true)
    if [ -n "$old" ]; then
        mountpoint -q "$LFS" && umount "$LFS"
        losetup -d "$old"
    fi
    rm -f "$IMAGE"
    truncate -s "$IMAGE_SIZE" "$IMAGE"
    echo 'start=2048, type=83, bootable' | sfdisk -q "$IMAGE"
    LOOP_DEV=$(losetup -fP --show "$IMAGE")
    ATTACHED=1
    PART_DEV=$(partition_dev "$LOOP_DEV")
    mkfs.ext4 -q -L lfs "$PART_DEV"
    info "partition $PART_DEV formatted as ext4"
    ensure_disk
    info "mounted at $LFS"
}

mount_vfs() {
    mkdir -p "$LFS"/{dev,proc,sys,run}
    mountpoint -q "$LFS/dev"     || mount --bind /dev "$LFS/dev"
    mountpoint -q "$LFS/dev/pts" || mount -t devpts devpts -o gid=5,mode=0620 "$LFS/dev/pts"
    mountpoint -q "$LFS/proc"    || mount -t proc proc "$LFS/proc"
    mountpoint -q "$LFS/sys"     || mount -t sysfs sysfs "$LFS/sys"
    mountpoint -q "$LFS/run"     || mount -t tmpfs tmpfs "$LFS/run"
    if [ -h "$LFS/dev/shm" ]; then
        install -d -m 1777 "$LFS$(realpath /dev/shm)"
    else
        mountpoint -q "$LFS/dev/shm" || mount -t tmpfs -o nosuid,nodev tmpfs "$LFS/dev/shm"
    fi
}

umount_vfs() {
    local m
    if findmnt -n "$LFS/etc/resolv.conf" >/dev/null 2>&1; then
        umount "$LFS/etc/resolv.conf"
    fi
    for m in dev/shm dev/pts sys proc run dev; do
        mountpoint -q "$LFS/$m" 2>/dev/null && umount "$LFS/$m"
    done
    return 0
}

cleanup() {
    local rc=$?
    umount_vfs || true
    if [ "$ATTACHED" = 1 ]; then
        mountpoint -q "$LFS" 2>/dev/null && umount "$LFS"
        [ -n "${LOOP_DEV:-}" ] && losetup -d "$LOOP_DEV" 2>/dev/null
    fi
    exit $rc
}
trap cleanup EXIT

# ---- Sources ---------------------------------------------------------------------------
phase_sources() {
    log "Downloading LFS $LFS_VERSION sources into $LFS/sources"
    mkdir -p "$LFS/sources"
    chmod a+wt "$LFS/sources"
    # The LFS mirror has every file in one place; the book's original sites are
    # the fallback. Timeouts keep a dead server from stalling the build.
    local url name missing=0 total done_n=0
    local get=(wget --continue --no-verbose --timeout=30 --tries=2 --waitretry=5)
    total=$(grep -c . "$WORK/book/wget-list-sysv")
    while read -r url; do
        [ -n "$url" ] || continue
        name=${url##*/}
        done_n=$((done_n + 1))
        if (cd "$LFS/sources" && grep " $name\$" "$WORK/book/md5sums" | md5sum -c --quiet >/dev/null 2>&1); then
            continue    # already downloaded and correct
        fi
        info "[$done_n/$total] $name"
        rm -f "$LFS/sources/$name"
        "${get[@]}" -O "$LFS/sources/$name" "$LFS_MIRROR/$name" 2>/dev/null \
            || "${get[@]}" -O "$LFS/sources/$name" "$url" 2>/dev/null \
            || { info "could not download $name from the mirror or $url"; rm -f "$LFS/sources/$name"; missing=1; }
    done < "$WORK/book/wget-list-sysv"
    [ "$missing" = 0 ] || die "some sources could not be downloaded (rerun to retry)"

    log "Checking MD5 sums"
    (cd "$LFS/sources" && md5sum -c --quiet "$WORK/book/md5sums") || die "MD5 check failed (see above)"
    info "all $(wc -l < "$WORK/book/md5sums") files OK"

    if [ "$WITH_NODE" = 1 ]; then
        log "Downloading Node.js $NODE_VERSION (for pe-inspect)"
        local base=https://nodejs.org/dist/v$NODE_VERSION tb=node-v$NODE_VERSION.tar.xz
        wget --continue --no-verbose -O "$LFS/sources/$tb" "$base/$tb"
        wget -q -O "$WORK/book/node-SHASUMS256.txt" "$base/SHASUMS256.txt"
        (cd "$LFS/sources" && grep " $tb\$" "$WORK/book/node-SHASUMS256.txt" | sha256sum -c --quiet) \
            || die "Node.js checksum failed"
    fi

    log "Copying the Windows-support package into the image"
    rm -rf "$LFS/sources/lfs-pe-inspect"
    cp -a "$WORK/lfs-pe-inspect" "$LFS/sources/lfs-pe-inspect"
    chown -R root:root "$LFS/sources"
    sync_build_dir
}

# ---- Chapter 4 -------------------------------------------------------------------------
phase_layout() {
    log "Creating the directory layout and the lfs user (chapter 4)"
    mkdir -p "$LFS"/{etc,var} "$LFS"/usr/{bin,lib,sbin} "$LFS/tools"
    local i
    for i in bin lib sbin; do
        [ -e "$LFS/$i" ] || ln -s "usr/$i" "$LFS/$i"
    done
    [ "$(uname -m)" = x86_64 ] && mkdir -p "$LFS/lib64"
    getent group lfs >/dev/null || groupadd lfs
    id lfs >/dev/null 2>&1 || useradd -s /bin/bash -g lfs -m -k /dev/null lfs
    chown lfs "$LFS"/{usr,usr/*,var,etc,tools}
    [ -d "$LFS/lib64" ] && chown lfs "$LFS/lib64"
    return 0
}

# ---- Running book sections -------------------------------------------------------------
run_section() {
    local mode=$1 num=$2
    case "$mode" in
        lfs)
            local path=$LFS/tools/bin:/usr/bin
            [ -L /bin ] || path=$path:/bin
            su lfs -s /bin/bash -c "exec env -i HOME=/home/lfs TERM=${TERM:-linux} \
                LFS=$LFS LC_ALL=POSIX LFS_TGT=$(uname -m)-lfs-linux-gnu PATH=$path \
                CONFIG_SITE=$LFS/usr/share/config.site MAKEFLAGS=-j$JOBS \
                BUILD_DIR=$LFS$IN_IMAGE_BUILD SRC_DIR=$LFS/sources \
                /bin/bash $LFS$IN_IMAGE_BUILD/run-section.sh $num" ;;
        host)
            env LFS="$LFS" BUILD_DIR="$LFS$IN_IMAGE_BUILD" SRC_DIR="$LFS/sources" \
                /bin/bash "$LFS$IN_IMAGE_BUILD/run-section.sh" "$num" ;;
        chroot)
            chroot "$LFS" /usr/bin/env -i HOME=/root TERM="${TERM:-linux}" \
                PATH=/usr/bin:/usr/sbin MAKEFLAGS="-j$JOBS" TESTSUITEFLAGS="-j$JOBS" \
                BUILD_DIR="$IN_IMAGE_BUILD" SRC_DIR=/sources \
                /bin/bash "$IN_IMAGE_BUILD/run-section.sh" "$num" ;;
    esac
}

run_chapter() {
    local chapter=$1 default_mode=$2
    local num ch id tarball flags title mode logf t0
    while IFS=$'\t' read -r num ch id tarball flags title; do
        [ "$ch" = "$chapter" ] || continue
        in_list "$id" "$SKIP_SECTIONS" && continue
        is_done "sec-$num" && continue
        mode=$default_mode
        in_list "$id" "$HOST_SECTIONS" && mode=host
        [ "$mode" = chroot ] && mount_vfs
        logf=$WORK/logs/$num-$id.log
        printf '    [%s] %-55s ' "$num" "$title"
        t0=$(date +%s)
        if [ "$VERBOSE" = 1 ]; then
            echo
            run_section "$mode" "$num" < /dev/null 2>&1 | tee "$logf" || { echo; tail -40 "$logf"; die "section $num failed; log: $logf"; }
        elif ! run_section "$mode" "$num" < /dev/null > "$logf" 2>&1; then
            echo "FAILED"
            tail -40 "$logf" | sed 's/^/      | /'
            die "section $num ($title) failed. Full log: $logf. Fix the cause and rerun to continue from here."
        fi
        printf '%4ss\n' "$(( $(date +%s) - t0 ))"
        mark_done "sec-$num"
    done < "$BUILD/manifest.tsv"
}

phase_cross()     { log "Chapter 5: cross toolchain (as user lfs)"; run_chapter 5 lfs; }
phase_temptools() { log "Chapter 6: temporary tools (as user lfs)"; run_chapter 6 lfs; }
phase_chroot()    { log "Chapter 7: chroot and more temporary tools"; run_chapter 7 chroot; }

phase_system() {
    log "Chapter 8: the final system"
    if [ -s "$ROOT_HASH" ]; then
        (umask 077; printf 'root:%s\n' "$(cat "$ROOT_HASH")" > "$LFS$IN_IMAGE_BUILD/.rootpw")
    fi
    run_chapter 8 chroot
    rm -f "$LFS$IN_IMAGE_BUILD/.rootpw" "$ROOT_HASH"
}

phase_config() { log "Chapter 9: system configuration"; run_chapter 9 chroot; }
phase_boot()   { log "Chapter 10: fstab, kernel, and GRUB"; run_chapter 10 chroot; }

phase_extras() {
    log "Installing Windows program support (pe-loader, pe-inspect) and release files"
    mount_vfs
    chroot "$LFS" /usr/bin/env -i HOME=/root TERM="${TERM:-linux}" PATH=/usr/bin:/usr/sbin \
        MAKEFLAGS="-j$JOBS" /bin/bash -e "$IN_IMAGE_BUILD/extras.sh" < /dev/null \
        > "$WORK/logs/extras.log" 2>&1 || { tail -40 "$WORK/logs/extras.log"; die "extras failed; log: $WORK/logs/extras.log"; }
    info "done (log: $WORK/logs/extras.log)"
}

# ---- Desktop: KDE Plasma from BLFS ------------------------------------------------------
BLFS_IN=/sources/blfs                 # seen from the chroot

blfs_plan() {
    log "Planning the KDE Plasma build from the BLFS $LFS_VERSION book"
    [ -s "$WORK/book/blfs.html" ] || {
        wget -q -O "$WORK/book/blfs.html.xz" "$BLFS_URL"
        xz -df "$WORK/book/blfs.html.xz"
    }
    local args=(--with "$BLFS_WITH" --lang "$LFS_LANG")
    [ "$KEEP_TESTS" = 1 ] && args+=(--keep-tests)
    rm -rf "$WORK/blfs-plan"
    # shellcheck disable=SC2086
    python3 "$SCRIPT_DIR/tools/blfs-book.py" "$WORK/book/blfs.html" "$WORK/blfs-plan" \
        $BLFS_TARGETS "${args[@]}" 2>&1 | sed 's/^/    /'

    # Edits for this build's choices (Xorg in /usr, boot to the login screen).
    local f
    for f in "$SCRIPT_DIR"/overrides/blfs-append/*.sh; do
        local id; id=$(basename "$f" .sh)
        local target; target=$(ls "$WORK/blfs-plan"/[0-9][0-9][0-9]-"$id".sh 2>/dev/null | head -1)
        [ -n "$target" ] || die "overrides/blfs-append/$id.sh: section $id is not in the plan"
        cat "$f" >> "$target"
    done
    f=$(ls "$WORK/blfs-plan"/[0-9][0-9][0-9]-sddm.sh)
    sed -i '/ServerPath.*opt\/xorg/d' "$f"     # the book assumes Xorg in /opt/xorg

    local B=$LFS$BLFS_IN
    mkdir -p "$B/cache"
    rm -rf "$B/plan"
    cp -a "$WORK/blfs-plan" "$B/plan"
    cp "$SCRIPT_DIR/tools/run-blfs-section.sh" "$SCRIPT_DIR/stages/desktop-env.sh" "$B/"
}

blfs_fetch() {
    log "Downloading BLFS sources (on the host; the chroot has no network tools)"
    local B=$LFS$BLFS_IN num id url md5 title cache name
    while IFS=$'\t' read -r num id url md5 title; do
        is_done "blfs-fetch-$num" && continue
        cache=$B/cache/$id
        mkdir -p "$cache"
        if [ "$url" != "-" ]; then
            name=${url##*/}
            wget --continue --no-verbose --tries=3 -O "$cache/$name" "$url" 2>&1 | grep -iE 'error|failed' | sed 's/^/    /' || true
            [ -s "$cache/$name" ] || die "could not download $url (section $id)"
            if [ "$md5" != "-" ]; then
                echo "$md5  $cache/$name" | md5sum -c --quiet || die "MD5 mismatch for $name (section $id)"
            fi
        fi
        if [ -f "$WORK/blfs-plan/$num-$id.extra" ]; then
            while read -r url; do
                [ -n "$url" ] || continue
                wget --continue --no-verbose -O "$cache/${url##*/}" "$url" || die "could not download $url (section $id)"
            done < "$WORK/blfs-plan/$num-$id.extra"
        fi
        if [ -f "$WORK/blfs-plan/$num-$id.fetch" ]; then
            # Bulk pages (Xorg, KF6, Plasma): run the book's own download commands.
            local dl=$WORK/blfs-dl/$id
            rm -rf "$dl"; mkdir -p "$dl"
            info "running the book's download steps for $id"
            (cd "$dl" && bash -e "$WORK/blfs-plan/$num-$id.fetch") > "$WORK/logs/blfs-fetch-$id.log" 2>&1 \
                || { tail -20 "$WORK/logs/blfs-fetch-$id.log"; die "download steps for $id failed"; }
            find "$dl" -type f -exec cp -f {} "$cache/" \;
        fi
        mark_done "blfs-fetch-$num"
    done < "$WORK/blfs-plan/plan.tsv"

    if ! is_done blfs-bootscripts; then
        local bs
        bs=$(grep -oE 'https?://[^"]*blfs-bootscripts-[0-9]+\.tar\.xz' "$WORK/book/blfs.html" | head -1)
        [ -n "$bs" ] || die "could not find the blfs-bootscripts URL in the BLFS book"
        wget -q -O "$WORK/book/${bs##*/}" "$bs"
        rm -rf "$B/bootscripts"; mkdir -p "$B/bootscripts"
        tar -xf "$WORK/book/${bs##*/}" -C "$B/bootscripts" --strip-components=1
        mark_done blfs-bootscripts
    fi
}

phase_desktop() {
    if [ "$DESKTOP" = none ]; then
        info "DESKTOP=none: no desktop to build"
        return
    fi
    blfs_plan
    blfs_fetch
    mount_vfs
    # Some builds (Rust crates, Python wheels) download at build time; borrow
    # the host's DNS settings for the duration.
    findmnt -n "$LFS/etc/resolv.conf" >/dev/null 2>&1 || mount --bind /etc/resolv.conf "$LFS/etc/resolv.conf"

    local chroot_env=(env -i HOME=/root TERM="${TERM:-linux}" PATH=/usr/bin:/usr/sbin
                      MAKEFLAGS="-j$JOBS" NINJAJOBS="$JOBS" BLFS_DIR="$BLFS_IN")
    if ! is_done blfs-env; then
        chroot "$LFS" "${chroot_env[@]}" /bin/bash -e "$BLFS_IN/desktop-env.sh" < /dev/null
        mark_done blfs-env
    fi

    log "Building KDE Plasma ($(wc -l < "$WORK/blfs-plan/plan.tsv") BLFS sections)"
    local num id url md5 title logf t0
    while IFS=$'\t' read -r num id url md5 title; do
        is_done "blfs-$num" && continue
        logf=$WORK/logs/blfs-$num-$id.log
        printf '    [%s] %-55s ' "$num" "$title"
        t0=$(date +%s)
        if ! chroot "$LFS" "${chroot_env[@]}" /bin/bash "$BLFS_IN/run-blfs-section.sh" "$num" \
                < /dev/null > "$logf" 2>&1; then
            echo "FAILED"
            tail -40 "$logf" | sed 's/^/      | /'
            die "BLFS section $num ($title) failed. Full log: $logf. Fix the cause and rerun to continue from here."
        fi
        printf '%5ss\n' "$(( $(date +%s) - t0 ))"
        mark_done "blfs-$num"
    done < "$WORK/blfs-plan/plan.tsv"
    umount "$LFS/etc/resolv.conf"
}

phase_finish() {
    log "LFS $LFS_VERSION is built: $IMAGE"
    cat <<EOF
    Boot it with QEMU (from Linux or WSL):
      qemu-system-x86_64 -m 4G -smp 4 -enable-kvm \\
        -drive file=$IMAGE,format=raw -nic user,model=e1000 -serial mon:stdio

    Or convert it for Hyper-V (Generation 1 VM, IDE disk):
      qemu-img convert -O vhdx $IMAGE lfs.vhdx

    Log in as root with the password you chose. Windows console programs run with:
      pe-loader program.exe        or, after boot, simply  ./program.exe
EOF
    if [ "$DESKTOP" != none ]; then
        cat <<EOF

    KDE Plasma: the login screen (SDDM) does not allow root. Before your first
    graphical login, press Ctrl+Alt+F2, log in as root, and create your user:
      useradd -m -G audio,video,input,users yourname
      passwd yourname
    Then press Ctrl+Alt+F7 (or reboot) and choose "Plasma (Wayland)".
    In QEMU, add a display for the desktop:  -device virtio-vga -display gtk
EOF
    fi
}

# ---- Main -----------------------------------------------------------------------------
NEEDS_DISK=" sources layout cross temptools chroot system config boot extras desktop "

for phase in "${PHASES[@]}"; do
    if [ "$phase" = preflight ]; then
        phase_preflight
    elif is_done "phase-$phase"; then
        info "skipping $phase (already done)"
    else
        in_list "$phase" "$NEEDS_DISK" && ensure_disk
        "phase_$phase"
        [ "$phase" = finish ] || mark_done "phase-$phase"
    fi
    # Recompose the scripts every run, so changes to overrides take effect on resume.
    if [ "$phase" = book ]; then
        compose_build_dir
    fi
    if [ "$phase" = "$STOP_AFTER" ]; then
        log "Stopping after '$phase' as asked. Rerun without --stop-after to continue."
        break
    fi
done
