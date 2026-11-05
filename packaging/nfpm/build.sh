#!/bin/sh
# Builds maicord_<version>_amd64.deb, maicord-<version>-1.x86_64.rpm and maicord-<version>-1-x86_64.pkg.tar.zst (Arch Linux)
# from a release tarball with nfpm.
#
#   packaging/nfpm/build.sh maicord-X.Y.Z-linux-x86_64.tar.gz OUTPUT-DIRECTORY
#
# Needs nfpm (https://nfpm.goreleaser.com/install/) on PATH. The version is the tarball's.
set -eu

[ "$#" -eq 2 ] || { sed -n '2,/^set -eu/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }
tarball="$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")"
here="$(cd "$(dirname "$0")" && pwd -P)"
mkdir -p "$2"
output="$(cd "$2" && pwd -P)"

name="$(basename "$tarball" .tar.gz)"
case "$name" in
    maicord-*-linux-x86_64) ;;
    *) echo "Not a MaiCord release tarball: $tarball" >&2; exit 2 ;;
esac
version="${name#maicord-}"
version="${version%-linux-x86_64}"

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
tar -xzf "$tarball" -C "$scratch"
cd "$scratch/$name"
for format in deb rpm; do
    VERSION="$version" nfpm package -f "$here/nfpm.yaml" -p "$format" -t "$output"
done
# The archlinux packager takes the time of the .MTREE entries and of the tar headers from two clock readings, which can
# differ by a second and show up as "Modification time mismatch" for the link in `pacman -Qkk`. A fixed time (the
# tarball's own, which build_tarball.py makes reproducible) makes them equal and the package reproducible.
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(stat -c %Y MaiCord)}" VERSION="$version" nfpm package -f "$here/nfpm.yaml" -p archlinux -t "$output"
