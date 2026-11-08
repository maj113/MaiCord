#!/usr/bin/env bash
# Builds the Linux port of MaiCord, audits what it built and, with --test, runs the Linux test suite. The options are
# tools/build.ps1's, spelled --kebab-case (-TestNames is --test-names), with the same meaning:
#
#   tools/build_linux.sh       configure, build the client (MaiCord), audit
#   --test                     also build and run the registered tests with ctest
#   --test-names a,b           only the tests named a and b (with --test)
#   --skip-client              tests use the MaiCord already in the build directory instead of building it (with --test)
#   --keep-intermediates       keep objects, ninja state and compile_commands.json (incremental builds)
#   --size-report              keep the client's symbol table (it ships stripped) and print its sections and largest
#                              symbols (tools/report_binary_size.py)
#   --llvm-bin DIR             the LLVM bin directory (default MAICORD_LLVM_BIN, else the pinned release)
#   --test-timeout-scale N     multiply every test timeout by N (slow hosted runners)
#
# From Windows: wsl -d Ubuntu-Preview --cd <checkout> -- bash -l tools/build_linux.sh [options].
#
# Layout and decisions (docs/ is git-ignored, so the reference lives here):
#
#   Host      Ubuntu 24.04+, Debian 13+ or the like (WSL "Ubuntu-Preview" for local work, ubuntu-24.04 in CI) with
#             cmake >= 3.28, ninja, python3 and, to sync, rsync (tools/install_linux_toolchain.sh installs them). The
#             compiler is the Linux release of the maj113/llvm-pe fork pinned in tools/llvm_toolchain.json, as on Windows:
#             clang, clang++, ld.lld, llvm-ar and llvm-ranlib of LLVM 24 (they need glibc 2.38), fetched once into
#             ~/.cache/maicord-toolchain/<tag> by tools/resolve_llvm_toolchain.sh and put first on PATH. --llvm-bin or
#             MAICORD_LLVM_BIN names another LLVM's bin directory instead and MAICORD_LLVM_MAJOR its major (default 24; any
#             other clang major is refused): a stock LLVM builds the same sources without the fork's code generation
#             options, which the contract takes only from a compiler that knows them; this is also how an older host
#             (Ubuntu 22.04, Debian 12) builds (MAICORD_LLVM_MAJOR=23 tools/build_linux.sh --llvm-bin /usr/lib/llvm-23/bin).
#             This is the only Linux entry point.
#   Source    On a native Linux file system (ext4, xfs, btrfs, tmpfs, overlay: a Linux checkout, GitHub Actions) the
#             checkout is built where it is. Where it sits on a Windows or network mount (WSL's /mnt/d is 9p, whose metadata
#             operations make ninja and git 50-700x slower) it is rsynced to ext4 first (default ~/mc; setting
#             MAICORD_LINUX_SOURCE always copies there; $CI never copies).
#   Build     ~/mc-build (MAICORD_LINUX_BUILD), one Release configuration: clang and ld.lld, full LTO, IPRA,
#             -fno-exceptions -fno-rtti, no unwind tables, -nostdinc with MaiCord's own headers
#             (include/maicord/sys/linux for the kernel and glibc interface), -mavx where Windows uses /arch:AVX.
#             cmake/build_contract.cmake is the contract; the client and the tests the run selects from the
#             test manifest (maicord-tests.txt, written by CMake as on Windows) are built. Size is the Windows client's:
#             cmake/size_policy.cmake (-Oz, -Os for what no measurement shows hot, and helper folding per source), the
#             machine outliner, --icf=all, --gc-sections, packed relocations, and the client is linked stripped
#             (--size-report keeps its symbol table for nm and profilers).
#   Image     A dynamic PIE with its own _start (src/core/linux/start.cpp): no CRT, raw syscalls, DT_NEEDED
#             libc.so.6 alone, as a generated stub (tools/generate_libc_stubs.py) that pins GLIBC_2.2.5, GLIBC_2.17
#             and GLIBC_2.34; every other library is dlopen()ed (include/maicord/core/shared_library.hpp).
#             The musl form is the same image with another program interpreter (tools/retarget_libc.py --libc musl).
#   Audit     tools/audit_image.py runs on every image: no GLIBC_ symbol version above 2.34, only the libc stub library
#             needed, no unwind tables, no TEXTREL, RPATH or RUNPATH, full RELRO, a non-executable stack. The client runs
#             it when it links (without a test hook unless the build has tests), and this script on its musl form
#             (--libc musl) and on the other images in the build directory.
#   Tests     ctest under the build directory; each case runs with a private XDG_DATA_HOME below it.
#   Cleanup   Unless --keep-intermediates, the run ends with the build directory holding only the client (MaiCord) and
#             its runtime assets (fonts/, licenses/), as tools/build.ps1 leaves the Windows one.
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
timeout_scale=1
llvm_major="${MAICORD_LLVM_MAJOR:-24}"
run_tests=0
keep=0
skip_client=0
size_report=OFF
test_names=()

value() {
    [ "$#" -ge 2 ] && [[ "$2" != --* ]] || { echo "$1 needs a value." >&2; exit 2; }
}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --test) run_tests=1; shift ;;
        --test-names) value "$@"; IFS=, read -r -a names <<< "$2"; test_names+=("${names[@]}"); shift 2 ;;
        --skip-client) skip_client=1; shift ;;
        --keep-intermediates) keep=1; shift ;;
        --size-report) size_report=ON; shift ;;
        --llvm-bin) value "$@"; export MAICORD_LLVM_BIN="$2"; shift 2 ;;
        --test-timeout-scale) value "$@"; timeout_scale="$2"; shift 2 ;;
        -h|--help) usage 0 ;;
        *) echo "Unknown argument: $1" >&2; usage 2 ;;
    esac
done
if [ "$run_tests" = 0 ] && { [ "${#test_names[@]}" -gt 0 ] || [ "$skip_client" = 1 ]; }; then
    echo "--test-names and --skip-client require --test." >&2
    exit 2
fi
for name in "${test_names[@]}"; do
    [[ "$name" =~ ^[A-Za-z0-9_.+-]+$ ]] || { echo "Invalid test name: $name" >&2; exit 2; }
done
if ! [[ "$timeout_scale" =~ ^[1-9][0-9]*$ ]]; then
    echo "Invalid timeout scale: $timeout_scale" >&2
    exit 2
fi

# Where the source is built from: this checkout when it is on a native Linux file system or in CI, else its ext4 copy.
sync=0
if [ -n "${MAICORD_LINUX_SOURCE:-}" ]; then
    sync=1
elif [ -z "${CI:-}" ] && [ -z "${GITHUB_ACTIONS:-}" ]; then
    case "$(stat -f -c %T "$repository" 2>/dev/null || echo unknown)" in
        9p|v9fs|drvfs|fuseblk|fuse|fuse.*|cifs|smb*|nfs*|virtiofs|vboxsf|ncpfs) sync=1 ;;
    esac
fi
if [ "$sync" = 1 ]; then
    source_directory="${source_directory:-$HOME/mc}"
else
    source_directory="$repository"
fi

# The toolchain: --llvm-bin or MAICORD_LLVM_BIN, else the pinned release (fetched on first use), first on PATH so a
# runner's own clang (ubuntu-24.04 ships 18) never wins.
llvm_bin="$(bash "$repository/tools/resolve_llvm_toolchain.sh")"
export PATH="$llvm_bin:$PATH"
tools=(cmake ninja python3)
[ "$sync" = 1 ] && tools+=(rsync)
for tool in "${tools[@]}"; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool (see the host notes in this script's header)." >&2; exit 1; }
done
clang_version="$("$llvm_bin/clang" -dumpfullversion 2>/dev/null || "$llvm_bin/clang" -dumpversion)"
if [ "${clang_version%%.*}" != "$llvm_major" ]; then
    echo "clang $llvm_major is required; $llvm_bin/clang is $clang_version (MAICORD_LLVM_MAJOR names another major)." >&2
    exit 1
fi
echo "Toolchain: clang $clang_version ($llvm_bin), $("$llvm_bin/ld.lld" --version | head -n 1), cmake $(cmake --version | head -n 1 | cut -d' ' -f3)"

# Concurrent invocations queue for the single build directory.
exec 9>"$HOME/.maicord-linux-build.lock"
if ! flock -n 9; then
    echo "Waiting for another MaiCord Linux build..."
    flock 9
fi

# A checkout on a slow mount is built from its ext4 copy; a run from that copy builds it in place.
if [ "$sync" = 1 ] && [ "$repository" != "$(cd "$source_directory" 2>/dev/null && pwd -P || true)" ]; then
    mkdir -p "$source_directory"
    rsync -a --delete --exclude .git --exclude Analysis --exclude docs --exclude .claude --exclude '*.log' \
        "$repository/" "$source_directory/"
fi

elapsed() {
    local milliseconds=$(( ($(date +%s%N) - $2) / 1000000 ))
    printf '%s: %d.%02d seconds %s\n' "$1" $((milliseconds / 1000)) $((milliseconds % 1000 / 10)) "$3"
}

clean() {
    # Leaves the client and the runtime assets beside it (fonts/, licenses/), as tools/build.ps1 leaves the Windows
    # directory with only the client and its assets; everything else goes. Also the start of a run, after one that was
    # interrupted before its cleanup.
    if [ "$keep" = 0 ] && [ -d "$build_directory" ]; then
        find "$build_directory" -mindepth 1 -maxdepth 1 ! -name MaiCord ! -name fonts ! -name licenses -exec rm -rf {} +
    fi
}
clean
trap clean EXIT

on() { if [ "$1" = 1 ]; then echo ON; else echo OFF; fi; }
cmake -S "$source_directory" -B "$build_directory" -G Ninja \
    -DCMAKE_C_COMPILER="$llvm_bin/clang" -DCMAKE_CXX_COMPILER="$llvm_bin/clang++" \
    -DCMAKE_AR="$llvm_bin/llvm-ar" -DCMAKE_RANLIB="$llvm_bin/llvm-ranlib" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMAICORD_BUILD_TESTS="$(on "$run_tests")" \
    -DMAICORD_TEST_PREBUILT_CLIENT="$(on "$skip_client")" \
    -DMAICORD_TEST_TIMEOUT_SCALE="$timeout_scale" \
    -DMAICORD_SIZE_REPORT="$size_report" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS="$(on "$keep")"

# The client (or, without it, the source rules it links after: tools/check_source_rules.py) and the executables of the
# selected tests (maicord-tests.txt: name, shard, labels and target, tab-separated).
if [ "$skip_client" = 1 ]; then targets=(maicord_source_rules); else targets=(MaiCord); fi
if [ "$run_tests" = 1 ]; then
    declare -A registered=() selected=() built=()
    for name in "${test_names[@]}"; do selected[$name]=1; done
    while IFS= read -r line; do
        name="${line%%$'\t'*}"
        target="${line##*$'\t'}"
        registered[$name]=1
        if [ -n "$target" ] && [ -z "${built[$target]:-}" ] && { [ "${#test_names[@]}" = 0 ] || [ -n "${selected[$name]:-}" ]; }; then
            built[$target]=1
            targets+=("$target")
        fi
    done < "$build_directory/maicord-tests.txt"
    for name in "${test_names[@]}"; do
        [ -n "${registered[$name]:-}" ] || { echo "Unknown registered test: $name" >&2; exit 2; }
    done
fi
started="$(date +%s%N)"
cmake --build "$build_directory" --parallel "$jobs" --target "${targets[@]}"
elapsed "Build" "$started" "($jobs workers)"

# The client audits itself when it links (tools/audit_image.py as its POST_BUILD step, without a test hook in a build
# without tests: that is the one that ships); here the other images in the build directory (the executables, not the
# stub libraries or archives) and the musl form of the client this run built (tools/retarget_libc.py: the same bytes
# with musl's interpreter).
images=()
while IFS= read -r -d '' file; do
    magic=
    [ "${file##*/}" = MaiCord ] || { IFS= LC_ALL=C read -r -n 4 magic < "$file" || true; }
    if [ "$magic" = $'\x7fELF' ]; then images+=("$file"); fi
done < <(find "$build_directory" -maxdepth 1 -type f -perm -u+x -print0)
[ "${#images[@]}" = 0 ] || python3 -B "$source_directory/tools/audit_image.py" "${images[@]}"
if [ "$skip_client" = 0 ]; then
    client="$build_directory/MaiCord"
    musl_client="$(mktemp)"
    if ! python3 -B "$source_directory/tools/retarget_libc.py" --libc musl "$client" "$musl_client" ||
            ! python3 -B "$source_directory/tools/audit_image.py" --libc musl "$musl_client"; then
        rm -f "$musl_client"
        exit 1
    fi
    rm -f "$musl_client"
    if [ "$size_report" = ON ]; then python3 -B "$source_directory/tools/report_binary_size.py" "$client"; fi
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
