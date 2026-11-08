#!/usr/bin/env bash
# Prints the bin directory of the LLVM toolchain the Linux build uses (clang, clang++, ld.lld, llvm-ar, llvm-ranlib).
# MAICORD_LLVM_BIN, when set, wins: any LLVM installation, the stock one included (/usr/lib/llvm-23/bin). Otherwise the
# Linux asset of the release pinned in tools/llvm_toolchain.json ("linux") is fetched once into
# $MAICORD_TOOLCHAIN_CACHE/<tag> (default ${XDG_CACHE_HOME:-~/.cache}/maicord-toolchain/<tag>), its SHA-256 checked,
# and reused. tools/resolve_llvm_toolchain.ps1 is the Windows twin; tools/build_linux.sh, tools/install_linux_toolchain.sh
# and CI call this, so local builds and CI use the same pin.
#
#   tools/resolve_llvm_toolchain.sh              print the bin directory, fetching the pinned release when it is missing
#   tools/resolve_llvm_toolchain.sh --no-fetch   print it only if it is already there
#
# Fetching needs curl, sha256sum, tar and xz. Exit status: nonzero, with the reason on standard error, without a toolchain.
set -euo pipefail

fetch=1
case "${1:-}" in
    "") ;;
    --no-fetch) fetch=0 ;;
    -h|--help) sed -n '2,/^# Fetching/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
esac

required=(clang clang++ ld.lld llvm-ar llvm-ranlib)
complete() {
    local tool
    for tool in "${required[@]}"; do [ -x "$1/$tool" ] || return 1; done
}

if [ -n "${MAICORD_LLVM_BIN:-}" ]; then
    complete "$MAICORD_LLVM_BIN" || { echo "The build needs ${required[*]} in $MAICORD_LLVM_BIN (MAICORD_LLVM_BIN)." >&2; exit 1; }
    cd "$MAICORD_LLVM_BIN" && pwd -P
    exit 0
fi

pin_file="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)/llvm_toolchain.json"
read -r repository tag asset sha256 < <(python3 -c '
import json, sys
pin = json.load(open(sys.argv[1], encoding="utf-8"))
print(pin["repository"], pin["tag"], pin["linux"]["asset"], pin["linux"]["sha256"])' "$pin_file")
[[ "$sha256" =~ ^[0-9a-fA-F]{64}$ ]] || { echo "tools/llvm_toolchain.json has no valid linux.sha256." >&2; exit 1; }
cache="${MAICORD_TOOLCHAIN_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/maicord-toolchain}"
root="$cache/$tag"
# The release's binaries need glibc 2.38 (Ubuntu 24.04, Debian 13 or newer); an older host builds with a stock LLVM.
usable() {
    "$root/bin/clang" --version >/dev/null 2>&1 || {
        echo "$root/bin/clang does not run here (the pinned release needs glibc 2.38 or newer): set MAICORD_LLVM_BIN to a stock LLVM." >&2
        exit 1
    }
    echo "$root/bin"
}
if complete "$root/bin"; then
    usable
    exit 0
fi
[ "$fetch" = 1 ] || { echo "The pinned LLVM toolchain $tag is not in $root (tools/install_linux_toolchain.sh fetches it)." >&2; exit 1; }
for tool in curl sha256sum tar xz; do
    command -v "$tool" >/dev/null || { echo "Fetching the pinned LLVM toolchain needs $tool." >&2; exit 1; }
done

# One fetch at a time per cache; a second caller finds the first one's result.
mkdir -p "$cache"
exec 8>"$cache/.lock"
flock 8
if ! complete "$root/bin"; then
    staging="$(mktemp -d "$cache/.$tag.XXXXXX")"
    trap 'rm -rf "$staging"' EXIT
    echo "Fetching the pinned LLVM toolchain $tag..." >&2
    curl --fail --silent --show-error --location --retry 5 \
        "https://github.com/$repository/releases/download/$tag/$asset" -o "$staging/$asset"
    hash="$(sha256sum "$staging/$asset" | cut -d ' ' -f 1)"
    if [ "$hash" != "${sha256,,}" ]; then
        echo "$asset from $tag hashes to $hash, not the pinned ${sha256,,}." >&2
        exit 1
    fi
    mkdir "$staging/toolchain"
    tar -xJf "$staging/$asset" -C "$staging/toolchain"
    complete "$staging/toolchain/bin" || { echo "$asset lacks ${required[*]} in bin." >&2; exit 1; }
    compgen -G "$staging/toolchain/lib/clang/*/include/stddef.h" >/dev/null ||
        { echo "$asset has no clang resource headers (lib/clang/<major>/include)." >&2; exit 1; }
    rm -rf "$root"
    mv "$staging/toolchain" "$root"
fi
usable
