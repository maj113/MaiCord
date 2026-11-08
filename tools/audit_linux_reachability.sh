#!/usr/bin/env bash
# Finds the code of the Linux client that no path from the entry point reaches, to see which features a stub or an #if
# cut off on Linux (and which are Windows-only on purpose).
#
#   tools/audit_linux_reachability.sh                 build a copy of the tree without LTO and report (default ~/mc-audit-build)
#   tools/audit_linux_reachability.sh --functions P   also list the dropped functions of the sources that contain P
#   tools/audit_linux_reachability.sh --top N         the N biggest sources (default 60)
#
# With link-time optimization (the shipping build) the compiler deletes unreachable functions before the linker sees them,
# so the audit builds the same sources with the contract's -flto switched off in a private copy (MAICORD_AUDIT_SOURCE,
# default ~/mc-audit-src; MAICORD_AUDIT_BUILD for the build directory), and links the objects twice:
#   --no-gc-sections -Map    every section of every archive member the link pulled in, with its size (the "built" set)
#   --gc-sections --print-gc-sections   the sections nothing reaches from _start (the "dropped" set)
# tools/audit_linux_reachability.py reads both and groups the dropped code by source file. A unit dropped whole, or a
# feature whose entry points are all dropped, is a path that Windows keeps and Linux does not: read the #if or the stub.
#
# Needs what tools/build_linux.sh needs (its LLVM toolchain, the pinned one or MAICORD_LLVM_BIN, cmake, ninja, rsync)
# and c++filt for the function names.

set -euo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
source_directory="${MAICORD_AUDIT_SOURCE:-$HOME/mc-audit-src}"
build_directory="${MAICORD_AUDIT_BUILD:-$HOME/mc-audit-build}"
report_arguments=()

while [ "$#" -gt 0 ]; do
    case "$1" in
        --functions) report_arguments+=(--functions "$2"); shift 2 ;;
        --top) report_arguments+=(--top "$2"); shift 2 ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

llvm_bin="$(bash "$repository/tools/resolve_llvm_toolchain.sh")"
export PATH="$llvm_bin:$PATH"

exec 9>"$HOME/.maicord-linux-build.lock"
flock 9
mkdir -p "$source_directory"
rsync -a --delete --exclude .git --exclude Analysis --exclude docs --exclude .claude --exclude '*.log' \
    "$repository/" "$source_directory/"
# The contract's whole-program switch is the only change to the copy.
sed -i 's/^set(MAICORD_WHOLE_PROGRAM_OPTION -flto)$/set(MAICORD_WHOLE_PROGRAM_OPTION "")/' \
    "$source_directory/cmake/build_contract.cmake"
grep -q '^set(MAICORD_WHOLE_PROGRAM_OPTION "")$' "$source_directory/cmake/build_contract.cmake" || {
    echo "The contract no longer has the LTO switch this script turns off." >&2; exit 1; }

cmake -S "$source_directory" -B "$build_directory" -G Ninja \
    -DCMAKE_C_COMPILER="$llvm_bin/clang" -DCMAKE_CXX_COMPILER="$llvm_bin/clang++" \
    -DCMAKE_AR="$llvm_bin/llvm-ar" -DCMAKE_RANLIB="$llvm_bin/llvm-ranlib" -DCMAKE_BUILD_TYPE=Release \
    -DMAICORD_BUILD_TESTS=OFF -DMAICORD_SIZE_REPORT=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > /dev/null
cmake --build "$build_directory" --target MaiCord --parallel "$(nproc)"

# The client's own link command, run again with another output and the audit's flags (the last of two
# --gc-sections / --no-gc-sections flags wins).
# ninja chains the link with the post-build steps ("cd ... && link && copy ..."): the link is the one with -o MaiCord.
link="$(ninja -C "$build_directory" -t commands MaiCord | grep -- ' -o MaiCord ' | tail -n 1 |
    sed 's/ && /\n/g' | grep -- ' -o MaiCord ' | head -n 1)"
[ -n "$link" ] || { echo "No link command for MaiCord in the build." >&2; exit 1; }
audit_directory="$build_directory/audit"
mkdir -p "$audit_directory"
relink() {  # output extra-flags...
    local output="$1"; shift
    local command="${link//-o MaiCord /-o $output }"
    [ "$command" != "$link" ] || { echo "The link command has no '-o MaiCord': $link" >&2; exit 1; }
    (cd "$build_directory" && eval "$command" "$@")
}
relink "audit/MaiCord.nogc" -Wl,--no-gc-sections "-Wl,-Map=audit/nogc.map"
# lld lists what it drops on its standard error and, with --print-gc-sections, on its output: keep both.
relink "audit/MaiCord.gc" -Wl,--gc-sections -Wl,--print-gc-sections "-Wl,-Map=audit/gc.map" > "$audit_directory/gc.log" 2>&1

python3 "$repository/tools/audit_linux_reachability.py" --map "$audit_directory/nogc.map" \
    --gc-log "$audit_directory/gc.log" --compile-commands "$build_directory/compile_commands.json" "${report_arguments[@]}"
