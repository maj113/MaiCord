#!/usr/bin/env bash
# Builds the Linux port of MaiCord, audits what it built and, with --test, runs the Linux test suite.
#
#   tools/build_linux.sh                       configure, build (core library), audit
#   tools/build_linux.sh --test                also build and run every registered test with ctest
#   tools/build_linux.sh --test a b            only the ctest cases named a and b
#   tools/build_linux.sh --test --keep         keep objects, ninja state and compile_commands.json (incremental)
#   tools/build_linux.sh --check               configure and build only; no audit, no tests
#
# From Windows use tools\build_linux.ps1, which runs this script inside WSL.
#
# Layout and decisions (docs/ is git-ignored, so the reference lives here):
#
#   Host      Ubuntu 22.04+ (WSL "Ubuntu-Preview" for local work) with clang and lld 23 from apt.llvm.org (clang,
#             clang++, ld.lld and llvm-ar on PATH, llvm-ranlib beside llvm-ar), cmake >= 3.28, ninja, python3 and rsync. The
#             Windows toolchain and tools/build.ps1 are unchanged; this is the only Linux entry point.
#   Source    The repository is rsynced to ext4 (default ~/mc, MAICORD_LINUX_SOURCE): /mnt/d is a 9p mount whose
#             metadata operations make ninja and git 50-700x slower. Never build in the /mnt tree.
#   Build     ~/mc-build (MAICORD_LINUX_BUILD), one Release configuration: stock clang and ld.lld, full LTO,
#             -fno-exceptions -fno-rtti, no unwind tables, -nostdinc with MaiCord's own headers
#             (include/maicord/sys/linux for the kernel and glibc interface), -mavx where Windows uses /arch:AVX.
#             cmake/build_contract_linux.cmake is the contract; cmake/linux_libraries.cmake and
#             cmake/linux_tests.cmake list what builds.
#   Image     A dynamic PIE with its own _start (src/core/linux/start.cpp): no CRT, raw syscalls, DT_NEEDED
#             libc.so.6 libdl.so.2 libpthread.so.0 as generated stubs (tools/generate_libc_stubs.py) that pin
#             GLIBC_2.2.5 and GLIBC_2.17; every other library is dlopen()ed (include/maicord/core/shared_library.hpp).
#   Audit     tools/audit_elf.py runs on every image: no GLIBC_ symbol version above 2.17, only the three stub
#             libraries needed, no unwind tables, no TEXTREL, RPATH or RUNPATH, full RELRO, a non-executable stack.
#   Tests     ctest under the build directory; each case runs with a private XDG_DATA_HOME below it.
#   Cleanup   Unless --keep, the run ends with the build directory holding only the shipping images (none yet),
#             as tools/build.ps1 leaves the Windows one with only the client.
#   Foreign   The images run on NixOS through nix-ld, or ld.so --library-path <glibc>:<libs> <image>.
#
# Exit status: nonzero on a failed configure, build, audit or test.

set -euo pipefail

usage() {
    sed -n '2,/^# Exit status/p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
source_directory="${MAICORD_LINUX_SOURCE:-$HOME/mc}"
build_directory="${MAICORD_LINUX_BUILD:-$HOME/mc-build}"
jobs="$(nproc)"
run_tests=0
keep=0
check_only=0
test_names=()

while [ "$#" -gt 0 ]; do
    case "$1" in
        --test)
            run_tests=1
            shift
            while [ "$#" -gt 0 ] && [[ "$1" != --* ]]; do
                if ! [[ "$1" =~ ^[A-Za-z0-9_.+-]+$ ]]; then
                    echo "Invalid test name: $1" >&2
                    exit 2
                fi
                test_names+=("$1")
                shift
            done
            ;;
        --keep) keep=1; shift ;;
        --check) check_only=1; shift ;;
        --jobs) jobs="$2"; shift 2 ;;
        -h|--help) usage 0 ;;
        *) echo "Unknown argument: $1" >&2; usage 2 ;;
    esac
done
if [ "$check_only" = 1 ] && [ "$run_tests" = 1 ]; then
    echo "--check builds only; it cannot run tests." >&2
    exit 2
fi

for tool in clang clang++ ld.lld llvm-ar cmake ninja python3 rsync; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool (see the host notes in this script's header)." >&2; exit 1; }
done
# apt.llvm.org installs llvm-ranlib in the LLVM directory without an unversioned alias on PATH.
llvm_ar="$(command -v llvm-ar)"
llvm_ranlib="$(command -v llvm-ranlib || echo "$(dirname "$(readlink -f "$llvm_ar")")/llvm-ranlib")"
[ -x "$llvm_ranlib" ] || { echo "Missing tool: llvm-ranlib." >&2; exit 1; }

# Concurrent invocations queue for the single build directory.
exec 9>"$HOME/.maicord-linux-build.lock"
if ! flock -n 9; then
    echo "Waiting for another MaiCord Linux build..."
    flock 9
fi

# The repository is built from its ext4 copy; a run from that copy builds it in place.
if [ "$repository" != "$(cd "$source_directory" 2>/dev/null && pwd -P || true)" ]; then
    mkdir -p "$source_directory"
    rsync -a --delete --exclude .git --exclude Analysis --exclude docs --exclude .claude --exclude '*.log' \
        "$repository/" "$source_directory/"
fi

elapsed() {
    local milliseconds=$(( ($(date +%s%N) - $2) / 1000000 ))
    printf '%s: %d.%02d seconds %s\n' "$1" $((milliseconds / 1000)) $((milliseconds % 1000 / 10)) "$3"
}

clean() {
    # Leaves only the shipping images: none yet, so the directory is emptied.
    if [ "$keep" = 0 ] && [ -d "$build_directory" ]; then
        find "$build_directory" -mindepth 1 -delete
    fi
}
trap clean EXIT

if [ "$keep" = 0 ] && [ -d "$build_directory" ]; then
    # A previous run that was interrupted before its cleanup.
    find "$build_directory" -mindepth 1 -delete
fi

cmake -S "$source_directory" -B "$build_directory" -G Ninja \
    -DCMAKE_C_COMPILER="$(command -v clang)" -DCMAKE_CXX_COMPILER="$(command -v clang++)" \
    -DCMAKE_AR="$llvm_ar" -DCMAKE_RANLIB="$llvm_ranlib" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMAICORD_BUILD_TESTS="$([ "$run_tests" = 1 ] && echo ON || echo OFF)" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS="$([ "$keep" = 1 ] && echo ON || echo OFF)"

started="$(date +%s%N)"
cmake --build "$build_directory" --parallel "$jobs"
elapsed "Build" "$started" "($jobs workers)"

if [ "$check_only" = 0 ]; then
    # Every image the build produced: the executables, not the stub libraries or archives.
    images=()
    while IFS= read -r -d '' file; do
        if [ "$(head -c 4 "$file")" = $'\x7fELF' ] && [ "$(dirname "$file")" = "$build_directory" ]; then
            images+=("$file")
        fi
    done < <(find "$build_directory" -maxdepth 1 -type f -perm -u+x -print0)
    if [ "${#images[@]}" -gt 0 ]; then
        python3 "$source_directory/tools/audit_elf.py" "${images[@]}"
    else
        echo "No images to audit."
    fi
fi

if [ "$run_tests" = 1 ]; then
    arguments=(--test-dir "$build_directory" --output-on-failure --parallel "$jobs")
    if [ "${#test_names[@]}" -gt 0 ]; then
        regex="^($(IFS='|'; echo "${test_names[*]}"))\$"
        arguments+=(--tests-regex "$regex")
    fi
    started="$(date +%s%N)"
    ctest "${arguments[@]}"
    elapsed "Tests passed in" "$started" ""
fi
