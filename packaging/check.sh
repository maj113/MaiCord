#!/bin/sh
# Checks the packaging that needs no client: builds the tarball around the stand-in program, validates the desktop entry
# and the AppStream metadata, checks that two builds are byte for byte the same, and installs and uninstalls the tarball
# under a private home (the link handler, the marker, the default-handler lines, the files left behind).
#
#   packaging/check.sh            needs python3, desktop-file-validate (desktop-file-utils) and appstreamcli (appstream)
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
echo "packaging checks passed for $tarball"
