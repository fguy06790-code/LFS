#!/bin/bash
# wget with timeouts, IPv4 first, and fallback locations for sources whose
# original site refuses, stalls or has moved them. build-lfs.sh puts this first
# in PATH as "wget" while downloading BLFS sources, so the book's own download
# commands get the same treatment.
real=/usr/bin/wget
opts=(--timeout=30 --tries=2 --prefer-family=IPv4)

"$real" "${opts[@]}" "$@" && exit 0
rc=$?

# Other places the same file can be found, one per line.
alternatives() {
    local url=$1 file=${1##*/} rest
    case $url in
        http://ftp.gnu.org/gnu/*|https://ftp.gnu.org/gnu/*|ftp://ftp.gnu.org/gnu/*)
            rest=${url#*://ftp.gnu.org/gnu/}
            echo "https://ftpmirror.gnu.org/gnu/$rest"
            echo "https://mirrors.kernel.org/gnu/$rest"
            echo "https://mirrors.ocf.berkeley.edu/gnu/$rest" ;;
        http://www.docbook.org/*|https://www.docbook.org/*)
            echo "https://archive.docbook.org/${url#*://www.docbook.org/}" ;;
    esac
    # BLFS's own source archive, by package name (file name up to "-<version>").
    local pkg
    pkg=$(echo "$file" | sed -E 's/[-_][0-9].*$//; s/\.(tar|zip|tgz).*$//' | tr 'A-Z' 'a-z')
    [ -n "$pkg" ] && [ -n "$file" ] && echo "https://ftp.osuosl.org/pub/blfs/conglomeration/$pkg/$file"
}

# Find the URL arguments; only single-URL calls are retried elsewhere.
urls=() idx=-1 i=0
for a in "$@"; do
    case $a in http://*|https://*|ftp://*) urls+=("$a"); idx=$i ;; esac
    i=$((i + 1))
done
[ "${#urls[@]}" = 1 ] || exit "$rc"
case ${urls[0]} in */) exit "$rc" ;; esac

args=("$@")
while read -r alt; do
    [ -n "$alt" ] || continue
    echo "    retrying from $alt" >&2
    args[$idx]=$alt
    "$real" "${opts[@]}" "${args[@]}" && exit 0
done < <(alternatives "${urls[0]}")
exit "$rc"
