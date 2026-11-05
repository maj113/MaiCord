#!/usr/bin/env bash
# Runs the portable subset of the Linux tests (tools/linux_portable_tests.txt) inside distribution containers: the same
# executables, built once on Ubuntu, on Ubuntu 22.04, Debian 12, Fedora, Arch Linux and NixOS (glibc) and on Alpine, Void
# and Chimera (musl). That is the proof of the one binary claim (D1): a freestanding image that needs only libc, libdl
# and libpthread and opens everything else by soname finds its libraries wherever the distribution keeps them, and copes
# where one is missing. The musl containers run the same bytes but for the program interpreter (the bundle's copy is
# retargeted with tools/retarget_libc.py: musl's loader answers the needed libc.so.6, libdl.so.2 and libpthread.so.0 by
# itself), and Alpine also compiles tools/check_libc_abi.py against musl's headers.
#
#   tools/test_linux_distros.sh --build ~/mc-build                  pack the cases from a build, run every distribution
#   tools/test_linux_distros.sh --build ~/mc-build debian-12 fedora only those
#   tools/test_linux_distros.sh --bundle DIR ubuntu-22.04            run a bundle tools/linux_portable_tests.py made
#   tools/test_linux_distros.sh --list                               the distributions
#   options: --engine docker|podman (default: docker, else podman; MAICORD_CONTAINER_ENGINE)
#            --out DIR  work directory and logs (default ~/mc-distro-test)
#            --client PATH  another client than the bundle's MaiCord (the build's client travels in the bundle: where the
#                           distribution installs sway, Alpine, it is also started on headless sway and its login
#                           screenshot kept as OUT/<distribution>-login.png; tools/test_client_wayland.sh)
#
# Each distribution is one container: a root step installs only the runtime libraries below (and the tools the cases
# call: openssl for the TLS server), then the cases run as an unprivileged user (uid 1000) with /mc/bundle (the
# executables, a private copy) and /mc/src (this repository, read-only: fixtures and the TLS smoke script) mounted. The
# containers use the host network (no tun device is needed, which rootless podman on WSL lacks). Exit status: nonzero
# when a case fails in any distribution; a distribution that cannot be set up (no network, no image) fails the run.
#
# The dlopen()ed libraries by distribution (soname: package). Fonts, hunspell and enchant are optional for the cases,
# which hold without them; the rest are what the Linux build opens:
#   libssl.so.3 / libcrypto.so.3     ubuntu, debian: libssl3 (libssl3t64 on 24.04) | fedora: openssl-libs | arch: openssl |
#                                    nixos: openssl.out
#   libfontconfig.so.1               libfontconfig1 | fontconfig | fontconfig | fontconfig.lib (+ a font: fonts-dejavu-core,
#                                    dejavu-sans-fonts, ttf-dejavu, dejavu_fonts)
#   libhunspell-1.7.so.0             libhunspell-1.7-0 + hunspell-en-us | hunspell + hunspell-en-US | hunspell +
#                                    hunspell-en_us | hunspell + hunspellDicts.en-us
#   libenchant-2.so.2                libenchant-2-2 | enchant2 | enchant | enchant
#   libpulse.so.0                    libpulse0 | pulseaudio-libs | libpulse | libpulseaudio
#   libEGL.so.1, libGLESv2.so.2      libegl1 libgles2 libegl-mesa0 libgl1-mesa-dri | mesa-libEGL mesa-libGLES
#                                    mesa-dri-drivers | mesa libglvnd | (nixos: not run, a container has no /run/opengl-driver)
#   libwayland-client.so.0 libwayland-egl.so.1 libwayland-cursor.so.0 libxkbcommon.so.0
#                                    libwayland-client0 libwayland-egl1 libwayland-cursor0 libxkbcommon0 | libwayland-client
#                                    libwayland-egl libwayland-cursor libxkbcommon | wayland libxkbcommon |
#                                    wayland libxkbcommon
#   Alpine (apk): mesa-egl mesa-gles mesa-dri-gallium wayland-libs-* libxkbcommon libpulse libhunspell hunspell-en
#   enchant2-libs fontconfig font-dejavu openssl; Void musl (xbps): mesa mesa-dri libglvnd wayland libxkbcommon
#   libpulseaudio hunspell enchant2 fontconfig dejavu-fonts-ttf openssl; Chimera (apk): mesa mesa-dri libglvnd libxkbcommon
#   libpulse hunspell-libs enchant fontconfig fonts-dejavu-ttf openssl3-libs.
#   NixOS: the glibc loader is linked at /lib64/ld-linux-x86-64.so.2 and LD_LIBRARY_PATH names the profile's lib directory,
#   which is what nix-ld does on a NixOS machine; a NixOS system with programs.nix-ld.enable does it for every program.
set -uo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
engine="${MAICORD_CONTAINER_ENGINE:-}"
out="$HOME/mc-distro-test"
build=""
bundle=""
selected=()
all_distros=(ubuntu-22.04 debian-12 fedora archlinux nixos alpine void-musl chimera)
client=""

image_of() {
    case "$1" in
        ubuntu-22.04) echo docker.io/library/ubuntu:22.04 ;;
        debian-12) echo docker.io/library/debian:12 ;;
        fedora) echo docker.io/library/fedora:latest ;;
        archlinux) echo docker.io/library/archlinux:latest ;;
        nixos) echo docker.io/nixos/nix:latest ;;
        alpine) echo docker.io/library/alpine:3.22 ;;
        void-musl) echo ghcr.io/void-linux/void-musl:latest ;;
        chimera) echo docker.io/chimeralinux/chimera:latest ;;
        *) return 1 ;;
    esac
}

# The C library of a distribution: musl ones run the bundle's executables retargeted.
libc_of() {
    case "$1" in
        alpine|void-musl|chimera) echo musl ;;
        *) echo glibc ;;
    esac
}

# The root step of a distribution: its runtime libraries.
# shellcheck disable=SC2016,SC2028 # the text is a script for the container's shell: nothing expands here
install_of() {
    case "$1" in
        ubuntu-22.04|debian-12)
            echo 'export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends ca-certificates openssl libssl3 libfontconfig1 fonts-dejavu-core \
    libhunspell-1.7-0 hunspell-en-us libenchant-2-2 libpulse0 libegl1 libgles2 libegl-mesa0 libgl1-mesa-dri \
    libwayland-client0 libwayland-egl1 libwayland-cursor0 libxkbcommon0 tzdata >/dev/null' ;;
        fedora)
            echo 'dnf -y -q install --setopt=install_weak_deps=False openssl fontconfig dejavu-sans-fonts hunspell \
    hunspell-en-US enchant2 pulseaudio-libs mesa-libEGL mesa-libGLES mesa-dri-drivers libwayland-client \
    libwayland-egl libwayland-cursor libxkbcommon ca-certificates >/dev/null' ;;
        archlinux)
            echo 'pacman -Syu --noconfirm --needed --disable-sandbox openssl fontconfig ttf-dejavu hunspell hunspell-en_us \
    enchant libpulse mesa libglvnd wayland libxkbcommon ca-certificates >/dev/null' ;;
        nixos)
            echo 'nix-env --profile /nix/var/nix/profiles/portable -iA nixpkgs.bash nixpkgs.coreutils nixpkgs.gnused nixpkgs.gnugrep \
    nixpkgs.glibc.out nixpkgs.glibc.bin nixpkgs.openssl.out nixpkgs.openssl.bin nixpkgs.fontconfig.lib nixpkgs.dejavu_fonts \
    nixpkgs.hunspell.out nixpkgs.hunspellDicts.en-us nixpkgs.enchant.out nixpkgs.libpulseaudio.out nixpkgs.wayland.out \
    nixpkgs.libxkbcommon.out >/dev/null 2>&1
profile=/nix/var/nix/profiles/portable
mkdir -p /lib64 /etc/fonts
ln -sf "$profile/lib/ld-linux-x86-64.so.2" /lib64/ld-linux-x86-64.so.2
printf "<?xml version=\"1.0\"?><fontconfig><dir>%s/share/fonts</dir><cachedir>/tmp/fontconfig</cachedir></fontconfig>\n" "$profile" > /etc/fonts/fonts.conf' ;;
        alpine)
            echo 'apk add --no-cache coreutils openssl tzdata ca-certificates fontconfig font-dejavu libhunspell hunspell-en \
    enchant2-libs libpulse mesa-egl mesa-gles mesa-dri-gallium wayland-libs-client wayland-libs-egl wayland-libs-cursor \
    libxkbcommon gcc musl-dev python3 >/dev/null && fc-cache -f' ;;
        void-musl)
            echo 'xbps-install -Syu xbps >/dev/null; xbps-install -Sy coreutils openssl tzdata ca-certificates fontconfig \
    dejavu-fonts-ttf hunspell hunspell-en_US enchant2 libpulseaudio mesa mesa-dri libglvnd wayland libxkbcommon >/dev/null \
    && fc-cache -f' ;;
        chimera)
            echo 'apk add --no-interactive openssl3-libs openssl3 ca-certificates tzdb fontconfig fonts-dejavu-ttf hunspell-libs \
    hunspell-en enchant libpulse mesa mesa-dri libglvnd libxkbcommon wayland >/dev/null && fc-cache -f' ;;
    esac
}

# What the client run (--client) needs on top: a compositor and the screenshot tools. The package's sway carries file
# capabilities an unprivileged container user cannot exec, so a copy (which has none) goes first on the PATH.
client_install_of() {
    case "$1" in
        alpine)
            echo 'apk add --no-cache sway grim bash py3-pillow font-noto-emoji >/dev/null && cp -f /usr/bin/sway /usr/local/bin/sway \
    && fc-cache -f' ;;
        *) return 1 ;;
    esac
}

# The checks that need a compiler, in the distributions that install one: the C library's headers against ours.
abi_check_of() {
    case "$1" in
        alpine) echo 'python3 /mc/src/tools/check_libc_abi.py --compiler cc' ;;
        *) return 1 ;;
    esac
}

# Environment of the test step (docker/podman -e arguments): where the profile and the dictionaries are on NixOS.
environment_of() {
    environment=()
    case "$1" in
        nixos) environment=(-e PATH=/nix/var/nix/profiles/portable/bin
                            -e LD_LIBRARY_PATH=/nix/var/nix/profiles/portable/lib
                            -e FONTCONFIG_FILE=/etc/fonts/fonts.conf
                            -e DICPATH=/nix/var/nix/profiles/portable/share/hunspell
                            -e "SECTIONS=default wayland") ;;
    esac
}

# Files the unprivileged container user wrote belong to a subordinate uid under rootless podman (and to uid 1000 under
# docker): remove them from inside the user namespace, or with sudo.
remove_tree() {
    [ -e "$1" ] || return 0
    if [ "$engine" = podman ]; then
        podman unshare rm -rf "$1"
    else
        rm -rf "$1" 2>/dev/null || sudo -n rm -rf "$1"
    fi
}

usage() {
    sed -n '2,/^set -uo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build) build="$2"; shift 2 ;;
        --bundle) bundle="$2"; shift 2 ;;
        --engine) engine="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --client) client="$2"; shift 2 ;;
        --list) printf '%s\n' "${all_distros[@]}"; exit 0 ;;
        -h|--help) usage 0 ;;
        -*) echo "Unknown argument: $1" >&2; usage 2 ;;
        *) image_of "$1" >/dev/null || { echo "Unknown distribution: $1 (see --list)" >&2; exit 2; }; selected+=("$1"); shift ;;
    esac
done
[ "${#selected[@]}" -gt 0 ] || selected=("${all_distros[@]}")
if [ -z "$engine" ]; then
    engine="$(command -v docker >/dev/null && echo docker || echo podman)"
fi
command -v "$engine" >/dev/null || { echo "No container engine: install docker or podman (--engine)." >&2; exit 1; }
[ -n "$build" ] || [ -n "$bundle" ] || { echo "Give --build (a built Linux build directory) or --bundle." >&2; exit 2; }

mkdir -p "$out"
if [ -z "$bundle" ]; then
    bundle="$out/bundle"
    python3 "$repository/tools/linux_portable_tests.py" --build "$build" --output "$bundle" || exit 1
fi

status=0
declare -A results
for distro in "${selected[@]}"; do
    image="$(image_of "$distro")"
    work="$out/work-$distro"
    log="$out/$distro.log"
    remove_tree "$work"
    cp -a "$bundle" "$work"
    [ -z "$client" ] || cp "$client" "$work/MaiCord"
    if [ "$(libc_of "$distro")" = musl ]; then
        python3 "$repository/tools/retarget_libc.py" --libc musl --tree "$work/build" || { echo "retarget failed" >&2; exit 1; }
        [ ! -f "$work/MaiCord" ] || python3 "$repository/tools/retarget_libc.py" --libc musl "$work/MaiCord" "$work/MaiCord" ||
            { echo "retarget failed" >&2; exit 1; }
    fi
    run_client=0
    [ -f "$work/MaiCord" ] && client_install_of "$distro" >/dev/null && run_client=1
    mkdir -p "$work/home" "$work/build"
    # A bundle that went through an artifact upload has lost its executable bits.
    find "$work/build" -type f -exec chmod a+rx {} +
    chmod -R a+rwX "$work"
    echo "== $distro ($image)"
    name="maicord-$distro-$$"
    # io_uring is the network loop's only backend and the engines' default seccomp profiles refuse it (Docker EPERM,
    # Podman ENOSYS), so the containers run without the profile.
    {
        "$engine" pull -q "$image" >/dev/null &&
        "$engine" run -d --name "$name" --network=host --security-opt seccomp=unconfined -v "$work:/mc/bundle" -v "$repository:/mc/src:ro" "$image" sleep 3600 >/dev/null &&
        echo "image $("$engine" image inspect "$image" --format '{{.Id}}')" &&
        "$engine" exec "$name" sh -c "$(install_of "$distro")"
    } >"$log" 2>&1
    prepared=$?
    if [ "$prepared" -eq 0 ]; then
        environment_of "$distro"
        outcome=0
        if abi_check_of "$distro" >/dev/null; then
            "$engine" exec "$name" sh -c "$(abi_check_of "$distro")" >>"$log" 2>&1 || outcome=1
        fi
        "$engine" exec --user 1000:1000 -e HOME=/mc/bundle/home "${environment[@]}" "$name" \
            sh /mc/bundle/portable.sh >>"$log" 2>&1 || outcome=1
        if [ "$run_client" = 1 ]; then
            # The client on headless sway with a private profile: it must stay up, draw the login screen and write no
            # error to its log (tools/test_client_wayland.sh).
            "$engine" exec "$name" sh -c "$(client_install_of "$distro")" >>"$log" 2>&1 &&
                "$engine" exec --user 1000:1000 -e HOME=/mc/bundle/home "$name" bash /mc/src/tools/test_client_wayland.sh \
                    --binary /mc/bundle/MaiCord --out /mc/bundle/client-qa >>"$log" 2>&1 || outcome=1
            [ ! -f "$work/client-qa/login.png" ] || cp "$work/client-qa/login.png" "$out/$distro-login.png"
        fi
    else
        outcome=99
        echo "setup failed (exit $prepared); see $log" >&2
    fi
    "$engine" rm -f "$name" >/dev/null 2>&1
    grep -E "^(host:|image |PASS|SKIP|FAIL|[0-9]+ passed|libc ABI|screenshot|window:|idle )" "$log" | grep -vE "^PASS" | sed 's/^/   /'
    if [ "$outcome" -eq 0 ]; then
        results[$distro]="ok: $(grep -E '^[0-9]+ passed' "$log")"
    else
        results[$distro]="FAILED (exit $outcome), log $log"
        status=1
    fi
done
echo
echo "== results"
for distro in "${selected[@]}"; do
    printf '%-14s %s\n' "$distro" "${results[$distro]}"
done
exit "$status"
