#!/usr/bin/env bash
# Installs what tools/build_linux.sh needs on Ubuntu or Debian (a GitHub Actions ubuntu-24.04 runner, a fresh machine,
# WSL) the same way every time.
#
#   tools/install_linux_toolchain.sh                 clang, lld and llvm 23 from apt.llvm.org, cmake, ninja, python3, git
#   tools/install_linux_toolchain.sh --tests         also the runtime libraries, fonts, dictionaries and tools the test suite
#                                                    uses (below), and the headers of the ABI checkers
#   tools/install_linux_toolchain.sh --integration   also what the compositor, D-Bus and audio integration scripts use
#   tools/install_linux_toolchain.sh --check         install nothing: verify clang 23 is there and print the versions
#
# Pins (this script is their only home; Dependabot cannot see them):
#   LLVM major   23: apt.llvm.org's llvm-toolchain-<codename>-23 repository. That repository is a rolling branch (the
#                pool keeps only the newest 23.x build of each package, so an exact package version cannot be pinned for
#                longer than days); the major is the pin, tools/build_linux.sh refuses any other, and the resolved version
#                is printed and written to the job summary so a change of compiler is visible in the log.
#   Archive key  the fingerprint below. The key is fetched over https, its fingerprint is compared and only then installed
#                as the repository's signing key; nothing else is trusted by it (signed-by).
#
# Packages the test suite and the opt-in scripts need, by what the Linux build opens with dlopen (names for Debian and
# Ubuntu; AGENTS.md lists the Fedora, Arch and NixOS ones):
#   libssl.so.3 libcrypto.so.3        libssl3t64 (libssl3 before Ubuntu 24.04), openssl (the CLI: s_server for the TLS tests)
#   libfontconfig.so.1                libfontconfig1; fonts: fonts-dejavu-core fonts-noto-core fonts-noto-cjk
#                                     fonts-noto-color-emoji fonts-liberation (MAICORD_TEST_FONT_SET=ubuntu)
#   libhunspell-1.7.so.0              libhunspell-1.7-0, hunspell-en-us, hunspell-de-de
#   libenchant-2.so.2                 libenchant-2-2
#   libpulse.so.0                     libpulse0 (pulseaudio for the loopback integration test)
#   libspeechd.so.2                   libspeechd2, libspeechd-dev for the ABI check (speech-dispatcher, with its dummy module,
#                                     for tools/test_speechd.sh)
#   libEGL.so.1 libGLESv2.so.2        libegl1 libgles2 and Mesa's drivers libegl-mesa0 libgl1-mesa-dri (llvmpipe, no GPU)
#   libwayland-client.so.0 libwayland-egl.so.1 libwayland-cursor.so.0 libxkbcommon.so.0
#                                     libwayland-client0 libwayland-egl1 libwayland-cursor0 libxkbcommon0
#   integration                       sway grim dbus-daemon python3-dbus python3-gi python3-pil pulseaudio speech-dispatcher (tools/test_dbus_real.sh,
#                                     which needs a graphical session, also wants xdg-desktop-portal xdg-desktop-portal-gtk gnome-keyring)
#
# Needs apt-get; runs it through sudo when not root. Exit status: nonzero on any failure.
set -euo pipefail

llvm_major=23
key_fingerprint=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421
key_url=https://apt.llvm.org/llvm-snapshot.gpg.key
key_path=/etc/apt/keyrings/apt.llvm.org.asc

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

llvm_bin="/usr/lib/llvm-$llvm_major/bin"

report() {
    local clang="$llvm_bin/clang"
    [ -x "$clang" ] || { echo "clang $llvm_major is not installed ($llvm_bin/clang is missing)." >&2; return 1; }
    local summary
    summary="$("$clang" --version | head -n 1); $("$llvm_bin/ld.lld" --version | head -n 1); $(cmake --version | head -n 1); ninja $(ninja --version)"
    echo "$summary"
    if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
        printf '### Linux toolchain\n\n- %s\n' "${summary//; /$'\n'- }" >> "$GITHUB_STEP_SUMMARY"
    fi
    if [ -n "${GITHUB_PATH:-}" ]; then
        echo "$llvm_bin" >> "$GITHUB_PATH"
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
    *) echo "Unsupported distribution ${ID:-unknown}: use apt.llvm.org's packages for it or the distribution's own LLVM $llvm_major." >&2; exit 1 ;;
esac
codename="${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}"
[ -n "$codename" ] || { echo "No codename in /etc/os-release." >&2; exit 1; }

if [ "$(id -u)" = 0 ]; then
    as_root() { "$@"; }
else
    command -v sudo >/dev/null || { echo "Run as root or install sudo." >&2; exit 1; }
    as_root() { sudo "$@"; }
fi
export DEBIAN_FRONTEND=noninteractive
apt_install() { as_root apt-get install -y --no-install-recommends -o Acquire::Retries=5 "$@"; }

# The repository: its signing key by fingerprint, then the source list.
repository="https://apt.llvm.org/$codename/"
suite="llvm-toolchain-$codename-$llvm_major"
if ! grep -qs "$suite" /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources 2>/dev/null; then
    as_root apt-get update -o Acquire::Retries=5
    apt_install ca-certificates curl gnupg
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT
    curl --fail --silent --show-error --location --retry 5 "$key_url" -o "$scratch/key.asc"
    fingerprints="$(gpg --show-keys --with-colons "$scratch/key.asc" | awk -F: '$1 == "fpr" {print $10}')"
    if ! grep -qx "$key_fingerprint" <<<"$fingerprints"; then
        echo "The apt.llvm.org key has no fingerprint $key_fingerprint (found: ${fingerprints//$'\n'/ })." >&2
        exit 1
    fi
    as_root install -d -m 0755 /etc/apt/keyrings
    as_root install -m 0644 "$scratch/key.asc" "$key_path"
    echo "deb [signed-by=$key_path] $repository $suite main" | as_root tee "/etc/apt/sources.list.d/llvm-$llvm_major.list" >/dev/null
fi
as_root apt-get update -o Acquire::Retries=5

apt_install "clang-$llvm_major" "lld-$llvm_major" "llvm-$llvm_major" cmake ninja-build python3 git rsync
as_root apt-cache policy "clang-$llvm_major" | sed -n '1,3p'

if [ "$with_tests" = 1 ]; then
    # libssl3t64 is the Ubuntu 24.04 name of libssl3; the other is tried when it does not exist.
    libssl=libssl3t64
    apt-cache show "$libssl" >/dev/null 2>&1 || libssl=libssl3
    packages=("$libssl" openssl libfontconfig1 fonts-dejavu-core fonts-noto-core fonts-noto-cjk fonts-noto-color-emoji
              fonts-liberation libhunspell-1.7-0 hunspell-en-us hunspell-de-de libenchant-2-2 libpulse0 libegl1 libgles2
              libegl-mesa0 libgl1-mesa-dri libwayland-client0 libwayland-egl1 libwayland-cursor0 libxkbcommon0
              libssl-dev libpulse-dev libspeechd-dev libspeechd2)
    if [ "$with_integration" = 1 ]; then
        packages+=(sway grim dbus-daemon python3-dbus python3-gi python3-pil pulseaudio speech-dispatcher)
    fi
    apt_install "${packages[@]}"
fi

report
