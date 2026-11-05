#!/usr/bin/env bash
# Checks the Linux release artifacts in a directory (what release.yml's linux job builds, or what packaging/check.sh builds
# around the stand-in program): checksums, the tarballs, the AppImage (update information, contents, that its program is the
# tarball's), the .deb and .rpm and the one-line installer. Nothing is installed or run.
#
#   packaging/check_artifacts.sh DIST VERSION [--stub]
#
# Needs: sha256sum, tar, readelf, dpkg-deb, rpm, desktop-file-validate and appstreamcli; the AppImage is unpacked with its own
# --appimage-extract, which needs no FUSE. --stub: the program is the shell stand-in, so no ELF checks.
# shellcheck disable=SC2015 # "[ a ] && [ b ] || fail": the failure of either test is the report
set -euo pipefail

[ "$#" -ge 2 ] || { sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }
dist="$(cd "$1" && pwd -P)"
version="$2"
stub=0
[ "${3:-}" != --stub ] || stub=1
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
fail() { echo "check_artifacts: $*" >&2; exit 1; }

tarball="maicord-$version-linux-x86_64.tar.gz"
musl="maicord-$version-linux-x86_64-musl.tar.gz"
appimage="MaiCord-$version-x86_64.AppImage"
appimage_musl="MaiCord-$version-x86_64-musl.AppImage"
deb="maicord_${version}_amd64.deb"
rpm="maicord-$version-1.x86_64.rpm"
for file in "$tarball" "$tarball.sha256" "$musl" "$musl.sha256" "$appimage" "$appimage.sha256" "$appimage.zsync" \
    "$appimage_musl" "$appimage_musl.sha256" "$appimage_musl.zsync" "$deb" "$rpm" install.sh "maicord-$version-linux-SHA256SUMS.txt"; do
    [ -f "$dist/$file" ] || fail "$file is missing from $dist"
done

(cd "$dist" && sha256sum -c "maicord-$version-linux-SHA256SUMS.txt" >/dev/null) || fail "SHA256SUMS does not match the files"
for file in "$tarball" "$musl" "$appimage" "$appimage_musl"; do
    (cd "$dist" && sha256sum -c "$file.sha256" >/dev/null) || fail "$file.sha256 does not match"
done

# The tarballs: one directory, the program, the marker, and (for musl) the retargeted interpreter.
mkdir "$scratch/glibc" "$scratch/musl"
tar -xzf "$dist/$tarball" -C "$scratch/glibc"
tar -xzf "$dist/$musl" -C "$scratch/musl"
glibc="$scratch/glibc/maicord-$version-linux-x86_64"
musl_directory="$scratch/musl/maicord-$version-linux-x86_64-musl"
[ -x "$glibc/MaiCord" ] && [ -f "$glibc/.maicord-portable" ] && [ -x "$musl_directory/MaiCord" ] || fail "a tarball lacks a part"
if [ "$stub" = 0 ]; then
    readelf -l "$glibc/MaiCord" | grep -q 'ld-linux-x86-64.so.2' || fail "the glibc tarball's program has another interpreter"
    readelf -l "$musl_directory/MaiCord" | grep -q 'ld-musl-x86_64.so.1' || fail "the musl tarball's program is not retargeted"
fi

# The AppImage: its update information and its contents.
if [ "${version#*-dev.}" = "$version" ]; then
    expected='gh-releases-zsync|maj113|MaiCord|latest|MaiCord-*-x86_64.AppImage.zsync'
    [ "$("$dist/$appimage" --appimage-updateinformation | tr -d '\0')" = "$expected" ] || fail "the AppImage's update information is wrong"
    grep -q "^URL: $appimage\$" "$dist/$appimage.zsync" || fail "the .zsync does not name the AppImage"
    expected='gh-releases-zsync|maj113|MaiCord|latest|MaiCord-*-x86_64-musl.AppImage.zsync'
    [ "$("$dist/$appimage_musl" --appimage-updateinformation | tr -d '\0')" = "$expected" ] || fail "the musl AppImage's update information is wrong"
    grep -q "^URL: $appimage_musl\$" "$dist/$appimage_musl.zsync" || fail "the musl .zsync does not name the AppImage"
fi
(cd "$scratch" && "$dist/$appimage_musl" --appimage-extract >/dev/null)
cmp "$scratch/squashfs-root/usr/bin/MaiCord" "$musl_directory/MaiCord" || fail "the musl AppImage's program is not the musl tarball's"
rm -rf "$scratch/squashfs-root"
(cd "$scratch" && "$dist/$appimage" --appimage-extract >/dev/null)
root="$scratch/squashfs-root"
[ -x "$root/AppRun" ] && [ -x "$root/usr/bin/MaiCord" ] && [ -d "$root/usr/bin/fonts" ] && [ -d "$root/usr/bin/licenses" ] || fail "the AppImage lacks a part"
cmp "$root/usr/bin/MaiCord" "$glibc/MaiCord" || fail "the AppImage's program is not the tarball's"
desktop-file-validate "$root/io.github.maj113.MaiCord.desktop"
appstreamcli validate --no-net "$root/usr/share/metainfo/io.github.maj113.MaiCord.metainfo.xml" >/dev/null
[ -L "$root/.DirIcon" ] && [ -f "$root/io.github.maj113.MaiCord.png" ] || fail "the AppImage lacks its icon"

# The packages.
[ "$(dpkg-deb -f "$dist/$deb" Version)" = "$version" ] || fail "the .deb has another version"
dpkg-deb -c "$dist/$deb" >"$scratch/deb.list"
grep -q ' ./usr/lib/maicord/MaiCord$' "$scratch/deb.list" || fail "the .deb lacks the program"
[ "$(rpm -qp --queryformat '%{VERSION}' "$dist/$rpm")" = "$version" ] || fail "the .rpm has another version"
rpm -qpl "$dist/$rpm" >"$scratch/rpm.list"
grep -qx '/usr/lib/maicord/MaiCord' "$scratch/rpm.list" || fail "the .rpm lacks the program"

# The installer: no placeholder left, parses.
! grep -q '@[A-Z_]*@' "$dist/install.sh" || fail "install.sh has an unfilled placeholder"
sh -n "$dist/install.sh" || fail "install.sh does not parse"
echo "the Linux artifacts of $version are consistent"
