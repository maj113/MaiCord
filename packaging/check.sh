#!/bin/sh
# Checks the packaging that needs no client: builds the tarball around the stand-in program, validates the desktop entry
# and the AppStream metadata, checks that two builds are byte for byte the same, and installs and uninstalls the tarball
# under a private home (the link handler, the marker, the default-handler lines, the files left behind); then the AppImages
# (run unpacked and, where FUSE works, mounted; --integrate and --unintegrate), the one-line installer against a throwaway
# key (packaging/online/test_installer.py) and, when nfpm is on PATH, the .deb and .rpm and the whole artifact set
# (packaging/check_artifacts.sh).
#
#   packaging/check.sh            needs python3, openssl, desktop-file-validate (desktop-file-utils), appstreamcli (appstream),
#                                 mksquashfs (squashfs-tools) and zsyncmake (zsync); the AppImage runtime is downloaded once
#                                 (SHA-256 pinned in packaging/appimage/runtime.json); nfpm: packaging/nfpm/install_nfpm.sh
# shellcheck disable=SC2015 # "[ a ] && [ b ] || { report; exit 1; }": the report is the failure of either test
set -eu

root="$(cd "$(dirname "$0")/.." && pwd -P)"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
version="$(python3 -B "$root/packaging/build_tarball.py" --stub --output-dir "$scratch/one" | sed -n 's/^maicord-\([0-9.]*\)-linux.*/\1/p')"
python3 -B "$root/packaging/build_tarball.py" --stub --output-dir "$scratch/two" >/dev/null
tarball="maicord-$version-linux-x86_64.tar.gz"
cmp "$scratch/one/$tarball" "$scratch/two/$tarball" || { echo "the tarball is not reproducible" >&2; exit 1; }
(cd "$scratch/one" && sha256sum -c "$tarball.sha256")

mkdir "$scratch/unpacked"
tar -xzf "$scratch/one/$tarball" -C "$scratch/unpacked"
unpacked="$scratch/unpacked/maicord-$version-linux-x86_64"
desktop-file-validate "$unpacked/share/applications/io.github.maj113.MaiCord.desktop"
python3 -B "$root/tools/generate_desktop_entry.py" --validate "$unpacked/share/applications/io.github.maj113.MaiCord.desktop"
appstreamcli validate --no-net "$unpacked/share/metainfo/io.github.maj113.MaiCord.metainfo.xml"
[ -f "$unpacked/.maicord-portable" ] && [ -x "$unpacked/MaiCord" ] && [ -x "$unpacked/install.sh" ] || { echo "the tarball lacks a part" >&2; exit 1; }

home="$scratch/home"
mkdir "$home"
HOME="$home" XDG_CONFIG_HOME="$home/.config" "$unpacked/install.sh" >/dev/null
[ "$("$home/.local/bin/maicord" --version)" = "MaiCord $version (stub)" ] || { echo "the installed program does not run" >&2; exit 1; }
grep -q "^Exec=$home/.local/opt/MaiCord/MaiCord %U\$" "$home/.local/share/applications/io.github.maj113.MaiCord.desktop" ||
    { echo "the installed desktop entry does not name the program" >&2; exit 1; }
[ -f "$home/.local/opt/MaiCord/.maicord-portable" ] || { echo "the install lacks the marker" >&2; exit 1; }
# A second install over the first, then the uninstall: nothing of MaiCord is left.
HOME="$home" XDG_CONFIG_HOME="$home/.config" "$unpacked/install.sh" >/dev/null
HOME="$home" XDG_CONFIG_HOME="$home/.config" "$home/.local/opt/MaiCord/uninstall.sh" >/dev/null
left="$(find "$home" -type f -o -type l | grep -i maicord || true)"
[ -z "$left" ] || { echo "the uninstall left: $left" >&2; exit 1; }

# The AppImage around the stand-in: it runs unpacked (no FUSE needed) and, where FUSE works, mounted; the integration it
# writes names the AppImage file and is removed again, leaving nothing.
python3 -B "$root/packaging/build_tarball.py" --stub --libc musl --output-dir "$scratch/one" >/dev/null
python3 -B "$root/packaging/appimage/build_appimage.py" --tarball "$scratch/one/$tarball" --output-dir "$scratch/one" >/dev/null
python3 -B "$root/packaging/appimage/build_appimage.py" --tarball "$scratch/one/maicord-$version-linux-x86_64-musl.tar.gz" --output-dir "$scratch/one" >/dev/null
appimage="$scratch/one/MaiCord-$version-x86_64.AppImage"
[ "$(APPIMAGE_EXTRACT_AND_RUN=1 "$appimage" --version)" = "MaiCord $version (stub)" ] || { echo "the AppImage does not run unpacked" >&2; exit 1; }
if [ -c /dev/fuse ] && { command -v fusermount3 >/dev/null 2>&1 || command -v fusermount >/dev/null 2>&1; }; then
    [ "$("$appimage" --version)" = "MaiCord $version (stub)" ] || { echo "the mounted AppImage does not run" >&2; exit 1; }
fi
APPIMAGE="$appimage" HOME="$home" XDG_CONFIG_HOME="$home/.config" XDG_DATA_HOME="$home/.local/share" APPIMAGE_EXTRACT_AND_RUN=1 "$appimage" --integrate >/dev/null
grep -q "^Exec=\"$appimage\" %U\$" "$home/.local/share/applications/io.github.maj113.MaiCord.desktop" ||
    { echo "the integrated desktop entry does not name the AppImage" >&2; exit 1; }
desktop-file-validate "$home/.local/share/applications/io.github.maj113.MaiCord.desktop"
APPIMAGE="$appimage" HOME="$home" XDG_CONFIG_HOME="$home/.config" XDG_DATA_HOME="$home/.local/share" APPIMAGE_EXTRACT_AND_RUN=1 "$appimage" --unintegrate >/dev/null
left="$(find "$home" -type f -o -type l | grep -i maicord || true)"
[ -z "$left" ] || { echo "the AppImage's --unintegrate left: $left" >&2; exit 1; }

# The one-line installer against a throwaway key.
python3 -B "$root/packaging/online/test_installer.py" >/dev/null || { python3 -B "$root/packaging/online/test_installer.py" >&2; exit 1; }

# The .deb and .rpm and the consistency of the whole release set, when nfpm is installed (packaging/nfpm/install_nfpm.sh).
if command -v nfpm >/dev/null 2>&1; then
    sh "$root/packaging/nfpm/build.sh" "$scratch/one/$tarball" "$scratch/one"
    python3 -B "$root/packaging/online/render_installer.py" --repository maj113/MaiCord \
        --public-key "04$(printf '%0128d' 1)" --output "$scratch/one/install.sh" >/dev/null
    (cd "$scratch/one" && find . -maxdepth 1 -type f ! -name '*.sha256' ! -name 'maicord-*-SHA256SUMS.txt' -printf '%P\n' | sort |
        xargs sha256sum >"maicord-$version-linux-SHA256SUMS.txt")
    bash "$root/packaging/check_artifacts.sh" "$scratch/one" "$version" --stub
else
    echo "nfpm is not installed: the .deb and .rpm checks are skipped"
fi
echo "packaging checks passed for $tarball"
