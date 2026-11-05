#!/usr/bin/env bash
# Builds the Linux port of MaiCord, audits what it built and, with --test, runs the Linux test suite.
#
#   tools/build_linux.sh                       configure, build the client (MaiCord) and the libraries, audit
#   tools/build_linux.sh --test                also build and run every registered test with ctest
#   tools/build_linux.sh --test a b            only the ctest cases named a and b
#   tools/build_linux.sh --test --keep         keep objects, ninja state and compile_commands.json (incremental)
#   tools/build_linux.sh --symbols             keep the symbol table in MaiCord (nm, bloaty, profilers); it ships stripped
#   tools/build_linux.sh --check               configure and build only; no audit, no tests
#   tools/build_linux.sh --sync | --in-place   copy the repository to ext4 first / build the checkout where it is
#   tools/build_linux.sh --timeout-scale N     multiply every test timeout by N (slow hosted runners)
#
# From Windows use tools\build_linux.ps1, which runs this script inside WSL.
#
# Layout and decisions (docs/ is git-ignored, so the reference lives here):
#
#   Host      Ubuntu 22.04+, Debian 12+ or the like (WSL "Ubuntu-Preview" for local work, ubuntu-24.04 in CI) with clang
#             and lld 23 from apt.llvm.org (tools/install_linux_toolchain.sh installs them; /usr/lib/llvm-23/bin is
#             put first on PATH when it exists, and any other clang major is refused), cmake >= 3.28, ninja, python3 and,
#             to sync, rsync. The Windows toolchain and tools/build.ps1 are unchanged; this is the only Linux entry point.
#   Source    On a native Linux file system (ext4, xfs, btrfs, tmpfs, overlay: a Linux checkout, GitHub Actions) the
#             checkout is built where it is. Where it sits on a Windows or network mount (WSL's /mnt/d is 9p, whose metadata
#             operations make ninja and git 50-700x slower) it is rsynced to ext4 first (default ~/mc,
#             MAICORD_LINUX_SOURCE; setting that variable, or --sync, always copies; $CI and --in-place never do).
#   Build     ~/mc-build (MAICORD_LINUX_BUILD), one Release configuration: stock clang and ld.lld, full LTO,
#             -fno-exceptions -fno-rtti, no unwind tables, -nostdinc with MaiCord's own headers
#             (include/maicord/sys/linux for the kernel and glibc interface), -mavx where Windows uses /arch:AVX.
#             cmake/build_contract_linux.cmake is the contract; cmake/linux_libraries.cmake and
#             cmake/linux_tests.cmake list what builds. Size is the Windows client's: cmake/size_policy.cmake (-Oz and
#             helper folding per source), the machine outliner, --icf=all, --gc-sections, packed relocations, and the
#             client is linked stripped (--symbols keeps its symbol table for nm and profilers).
#   Image     A dynamic PIE with its own _start (src/core/linux/start.cpp): no CRT, raw syscalls, DT_NEEDED
#             libc.so.6 libdl.so.2 libpthread.so.0 as generated stubs (tools/generate_libc_stubs.py) that pin
#             GLIBC_2.2.5 and GLIBC_2.17; every other library is dlopen()ed (include/maicord/core/shared_library.hpp).
#   Audit     tools/audit_elf.py runs on every image: no GLIBC_ symbol version above 2.17, only the three stub
#             libraries needed, no unwind tables, no TEXTREL, RPATH or RUNPATH, full RELRO, a non-executable stack.
#   Tests     ctest under the build directory; each case runs with a private XDG_DATA_HOME below it.
#   Cleanup   Unless --keep, the run ends with the build directory holding only the shipping client (MaiCord) and its
#             runtime assets (fonts/, licenses/), as tools/build.ps1 leaves the Windows one with only the client.
#   Foreign   The images run on NixOS through nix-ld, or ld.so --library-path <glibc>:<libs> <image>.
#
# Exit status: nonzero on a failed configure, build, audit or test.

set -euo pipefail

usage() {
    sed -n '2,/^# Exit status/p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
source_directory="${MAICORD_LINUX_SOURCE:-}"
build_directory="${MAICORD_LINUX_BUILD:-$HOME/mc-build}"
jobs="$(nproc)"
sync_mode=auto
timeout_scale="${MAICORD_TEST_TIMEOUT_SCALE:-1}"
llvm_major="${MAICORD_LLVM_MAJOR:-23}"
run_tests=0
keep=0
check_only=0
symbols=OFF
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
        --symbols) symbols=ON; shift ;;
        --check) check_only=1; shift ;;
        --jobs) jobs="$2"; shift 2 ;;
        --sync) sync_mode=sync; shift ;;
        --in-place) sync_mode=in-place; shift ;;
        --timeout-scale) timeout_scale="$2"; shift 2 ;;
        -h|--help) usage 0 ;;
        *) echo "Unknown argument: $1" >&2; usage 2 ;;
    esac
done
if [ "$check_only" = 1 ] && [ "$run_tests" = 1 ]; then
    echo "--check builds only; it cannot run tests." >&2
    exit 2
fi
if ! [[ "$timeout_scale" =~ ^[1-9][0-9]*$ ]]; then
    echo "Invalid timeout scale: $timeout_scale" >&2
    exit 2
fi

# Where the source is built from: this checkout when it is on a native Linux file system or in CI, else its ext4 copy.
if [ "$sync_mode" = auto ]; then
    if [ -n "${MAICORD_LINUX_SOURCE:-}" ]; then
        sync_mode=sync
    elif [ -n "${CI:-}" ] || [ -n "${GITHUB_ACTIONS:-}" ]; then
        sync_mode=in-place
    else
        case "$(stat -f -c %T "$repository" 2>/dev/null || echo unknown)" in
            9p|v9fs|drvfs|fuseblk|fuse|fuse.*|cifs|smb*|nfs*|virtiofs|vboxsf|ncpfs) sync_mode=sync ;;
            *) sync_mode=in-place ;;
        esac
    fi
fi
if [ "$sync_mode" = sync ]; then
    source_directory="${source_directory:-$HOME/mc}"
else
    source_directory="$repository"
fi

# The LLVM major this port is built with; a runner's own older clang (ubuntu-24.04 ships 18) must not win.
if [ -d "/usr/lib/llvm-$llvm_major/bin" ]; then
    export PATH="/usr/lib/llvm-$llvm_major/bin:$PATH"
fi
tools=(clang clang++ ld.lld llvm-ar cmake ninja python3)
[ "$sync_mode" = sync ] && tools+=(rsync)
for tool in "${tools[@]}"; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool (see the host notes in this script's header)." >&2; exit 1; }
done
clang_version="$(clang -dumpfullversion 2>/dev/null || clang -dumpversion)"
if [ "${clang_version%%.*}" != "$llvm_major" ]; then
    echo "clang $llvm_major is required; $(command -v clang) is $clang_version (tools/install_linux_toolchain.sh installs it)." >&2
    exit 1
fi
echo "Toolchain: clang $clang_version, $(ld.lld --version | head -n 1), cmake $(cmake --version | head -n 1 | cut -d' ' -f3)"
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

# A checkout on a slow mount is built from its ext4 copy; a run from that copy builds it in place.
if [ "$sync_mode" = sync ] && [ "$repository" != "$(cd "$source_directory" 2>/dev/null && pwd -P || true)" ]; then
    mkdir -p "$source_directory"
    rsync -a --delete --exclude .git --exclude Analysis --exclude docs --exclude .claude --exclude '*.log' \
        "$repository/" "$source_directory/"
fi

elapsed() {
    local milliseconds=$(( ($(date +%s%N) - $2) / 1000000 ))
    printf '%s: %d.%02d seconds %s\n' "$1" $((milliseconds / 1000)) $((milliseconds % 1000 / 10)) "$3"
}

clean() {
    # Leaves the shipping client and the runtime assets beside it (fonts/, licenses/), as tools/build.ps1 leaves the
    # Windows directory with only the client and its assets; everything else goes.
    if [ "$keep" = 0 ] && [ -d "$build_directory" ]; then
        find "$build_directory" -mindepth 1 -maxdepth 1 ! -name MaiCord ! -name fonts ! -name licenses -exec rm -rf {} +
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
    -DMAICORD_TEST_TIMEOUT_SCALE="$timeout_scale" \
    -DMAICORD_KEEP_SYMBOLS="$symbols" \
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
    # The Linux file seam holds a descriptor per open file (advisory sharing), and the font session test opens more than
    # a thousand: a container's or a login shell's soft limit of 1024 is raised to the hard one, at most 65536.
    hard_limit="$(ulimit -Hn)"
    if [ "$hard_limit" = unlimited ] || [ "$hard_limit" -gt 65536 ]; then hard_limit=65536; fi
    [ "$(ulimit -Sn)" = unlimited ] || [ "$(ulimit -Sn)" -ge "$hard_limit" ] || ulimit -Sn "$hard_limit" || true
    arguments=(--test-dir "$build_directory" --output-on-failure --parallel "$jobs")
    if [ "${#test_names[@]}" -gt 0 ]; then
        regex="^($(IFS='|'; echo "${test_names[*]}"))\$"
        arguments+=(--tests-regex "$regex")
    fi
    started="$(date +%s%N)"
    ctest "${arguments[@]}"
    elapsed "Tests passed in" "$started" ""
fi
