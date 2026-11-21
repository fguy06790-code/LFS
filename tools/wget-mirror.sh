#!/bin/bash
# wget with timeouts, IPv4 first, and fallback mirrors for ftp.gnu.org, which
# often refuses or stalls connections. build-lfs.sh puts this first in PATH as
# "wget" while downloading BLFS sources, so the book's own download commands
# get the same treatment.
real=/usr/bin/wget
opts=(--timeout=30 --tries=2 --prefer-family=IPv4)

"$real" "${opts[@]}" "$@" && exit 0
rc=$?

for mirror in https://ftpmirror.gnu.org/gnu/ https://mirrors.kernel.org/gnu/ \
              https://mirrors.ocf.berkeley.edu/gnu/; do
    new=() changed=0
    for a in "$@"; do
        case $a in
            http://ftp.gnu.org/gnu/*|https://ftp.gnu.org/gnu/*|ftp://ftp.gnu.org/gnu/*)
                new+=("$mirror${a#*://ftp.gnu.org/gnu/}"); changed=1 ;;
            *)  new+=("$a") ;;
        esac
    done
    [ "$changed" = 1 ] || exit "$rc"
    echo "    retrying from $mirror" >&2
    "$real" "${opts[@]}" "${new[@]}" && exit 0
done
exit "$rc"
