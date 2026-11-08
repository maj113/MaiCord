#!/usr/bin/env bash
# Installs what tools/build_linux.sh needs on Ubuntu or Debian (a GitHub Actions ubuntu-24.04 runner, a fresh machine,
# WSL) the same way every time.
#
#   tools/install_linux_toolchain.sh                 the pinned LLVM toolchain (below), cmake, ninja, python3, git, rsync and
#                                                    what CMake's and the ABI checks' hosted programs link with
#   tools/install_linux_toolchain.sh --tests         also the runtime libraries, fonts, dictionaries and tools the test suite
#                                                    uses (below), and the headers of the ABI checkers
#   tools/install_linux_toolchain.sh --integration   also what the compositor, D-Bus and audio integration scripts use
#   tools/install_linux_toolchain.sh --check         install nothing: verify the toolchain is there and print the versions
#
# The toolchain is the Linux asset of the maj113/llvm-pe release pinned in tools/llvm_toolchain.json (clang, clang++,
# ld.lld, llvm-ar and llvm-ranlib, LLVM 24): tools/resolve_llvm_toolchain.sh fetches it once, checks its SHA-256 and
# unpacks it into ~/.cache/maicord-toolchain/<tag> (MAICORD_TOOLCHAIN_CACHE, else XDG_CACHE_HOME), as
# tools/resolve_llvm_toolchain.ps1 does the Windows one; the Windows and Linux assets come from the same tag. Its clang
# links hosted programs with the system's linker, C library and GCC runtime, which come from apt:
#   binutils                          ld, for CMake's compiler checks and tools/check_video_abi.py's probe programs
#   libc6-dev                         crt1.o and libc.so for those links; the headers the libc ABI check reads
#   libgcc-<N>-dev libstdc++-<N>-dev  crtbegin.o, libgcc and libstdc++ of the distribution's default GCC <N>, for CMake's C++
#                                     compiler check (clang takes the GCC installation it finds)
# Its binaries need glibc 2.38 (Ubuntu 24.04, Debian 13 or newer); on an older system, with MAICORD_LLVM_BIN set to a stock
# LLVM (tools/build_linux.sh), this script installs the packages and leaves the toolchain alone.
#
# Packages the test suite and the opt-in scripts need, by what the Linux build opens with dlopen (names for Debian and
# Ubuntu; tools/test_linux_distros.sh lists the Fedora, Arch and NixOS ones):
#   libssl.so.3 libcrypto.so.3        libssl3t64 (libssl3 before Ubuntu 24.04), openssl (the CLI: s_server for the TLS tests)
#   libfontconfig.so.1                libfontconfig1; fonts: fonts-dejavu-core fonts-noto-core fonts-noto-cjk
#                                     fonts-noto-color-emoji fonts-liberation (MAICORD_TEST_FONT_SET=ubuntu)
#   libhunspell-1.7.so.0              libhunspell-1.7-0, hunspell-en-us, hunspell-de-de
#   libenchant-2.so.2                 libenchant-2-2
#   libpulse.so.0                     libpulse0 (pulseaudio for the loopback integration test)
#   libspeechd.so.2                   libspeechd2, libspeechd-dev for the ABI check (speech-dispatcher, with its dummy module,
#                                     for tools/test_speechd.sh)
#   libfido2.so.1                     libfido2-1, libfido2-dev for the ABI check (optional at run time: security keys)
#   libva.so.2 libva-drm.so.2         libva2 libva-drm2 (libva-dev: the ABI check), libdav1d.so.7 (libdav1d-dev),
#                                     libopenh264.so.7 (libopenh264-dev): the video tiers' libraries and the headers
#                                     tools/check_video_abi.py compares their declarations with
#   libEGL.so.1 libGLESv2.so.2        libegl1 libgles2 and Mesa's drivers libegl-mesa0 libgl1-mesa-dri (llvmpipe, no GPU)
#   libwayland-client.so.0 libwayland-egl.so.1 libwayland-cursor.so.0 libxkbcommon.so.0
#                                     libwayland-client0 libwayland-egl1 libwayland-cursor0 libxkbcommon0
#   integration                       sway grim dbus-daemon python3-dbus python3-gi python3-pil pulseaudio speech-dispatcher
#                                     at-spi2-core gir1.2-atspi-2.0 (tools/test_linux_integration.sh), appstream,
#                                     desktop-file-utils, squashfs-tools, zsync and rpm (packaging/check.sh); tools/test_dbus_real.sh, which needs a graphical
#                                     session, also wants xdg-desktop-portal xdg-desktop-portal-gtk gnome-keyring
#
# Needs apt-get; runs it through sudo when not root. Exit status: nonzero on any failure.
set -euo pipefail

resolve="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)/resolve_llvm_toolchain.sh"

with_tests=0
with_integration=0
check_only=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --tests) with_tests=1; shift ;;
        --integration) with_tests=1; with_integration=1; shift ;;
        --check) check_only=1; shift ;;
        -h|--help) sed -n '2,/^# Needs apt-get/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

# The versions, also in the job summary so a change of compiler is visible in the log; in CI the bin directory goes on
# PATH and into MAICORD_LLVM_BIN for the later steps, as the Windows setup action does.
report() {
    local llvm_bin summary
    llvm_bin="$(bash "$resolve" --no-fetch)"
    summary="$("$llvm_bin/clang" --version | head -n 1); $("$llvm_bin/ld.lld" --version | head -n 1); $(cmake --version | head -n 1); ninja $(ninja --version)"
    echo "$summary"
    if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
        printf '### Linux toolchain\n\n- %s\n' "${summary//; /$'\n'- }" >> "$GITHUB_STEP_SUMMARY"
    fi
    if [ -n "${GITHUB_PATH:-}" ]; then
        echo "$llvm_bin" >> "$GITHUB_PATH"
    fi
    if [ -n "${GITHUB_ENV:-}" ]; then
        echo "MAICORD_LLVM_BIN=$llvm_bin" >> "$GITHUB_ENV"
    fi
}

if [ "$check_only" = 1 ]; then
    report
    exit
fi

[ -r /etc/os-release ] || { echo "No /etc/os-release: only Debian and Ubuntu are supported here." >&2; exit 1; }
# shellcheck disable=SC1091
. /etc/os-release
case "${ID:-}:${ID_LIKE:-}" in
    ubuntu:*|debian:*|*:*debian*|*:*ubuntu*) ;;
    *) echo "Unsupported distribution ${ID:-unknown}: install the packages above by hand; tools/build_linux.sh fetches the toolchain." >&2; exit 1 ;;
esac

if [ "$(id -u)" = 0 ]; then
    as_root() { "$@"; }
else
    command -v sudo >/dev/null || { echo "Run as root or install sudo." >&2; exit 1; }
    as_root() { sudo "$@"; }
fi
export DEBIAN_FRONTEND=noninteractive
apt_install() { as_root apt-get install -y --no-install-recommends -o Acquire::Retries=5 "$@"; }

as_root apt-get update -o Acquire::Retries=5
# The distribution's default GCC: the gcc package depends on gcc-<N>.
gcc_major="$(apt-cache depends gcc | sed -n 's/^ *Depends: gcc-\([0-9][0-9]*\)$/\1/p' | head -n 1)"
[ -n "$gcc_major" ] || { echo "No default GCC version in the package lists (apt-cache depends gcc)." >&2; exit 1; }
apt_install cmake ninja-build python3 git rsync ca-certificates curl xz-utils \
    binutils libc6-dev "libgcc-$gcc_major-dev" "libstdc++-$gcc_major-dev"
bash "$resolve" > /dev/null

if [ "$with_tests" = 1 ]; then
    # libssl3t64 is the Ubuntu 24.04 name of libssl3; the other is tried when it does not exist.
    libssl=libssl3t64
    apt-cache show "$libssl" >/dev/null 2>&1 || libssl=libssl3
    packages=("$libssl" openssl libfontconfig1 fonts-dejavu-core fonts-noto-core fonts-noto-cjk fonts-noto-color-emoji
              fonts-liberation libhunspell-1.7-0 hunspell-en-us hunspell-de-de libenchant-2-2 libpulse0 libegl1 libgles2
              libegl-mesa0 libgl1-mesa-dri libwayland-client0 libwayland-egl1 libwayland-cursor0 libxkbcommon0
              libssl-dev libpulse-dev libspeechd-dev libspeechd2 libfido2-dev libfido2-1 libva-dev libdav1d-dev
              libopenh264-dev)
    if [ "$with_integration" = 1 ]; then
        packages+=(sway grim dbus-daemon python3-dbus python3-gi python3-pil pulseaudio speech-dispatcher
                   at-spi2-core gir1.2-atspi-2.0 appstream desktop-file-utils squashfs-tools zsync rpm)
    fi
    apt_install "${packages[@]}"
fi

report
