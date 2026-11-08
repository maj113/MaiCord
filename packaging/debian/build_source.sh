#!/bin/sh
# Makes the Debian source package of a MaiCord release for a Launchpad PPA (or `dpkg-buildpackage` anywhere), from the release
# tarball: the upstream program is built with LLVM 24, which an Ubuntu builder does not have, so the source package carries
# the tarball (as maicord_X.Y.Z.orig.tar.gz) and the debian/ directory installs its contents. Nothing is compiled.
#
#   packaging/debian/build_source.sh --tarball maicord-X.Y.Z-linux-x86_64.tar.gz --series noble --maintainer 'Name <you@example.org>'
#       [--revision 1] [--output DIR]       (the maintainer is the identity of the GPG key you sign the upload with)
#
# Writes maicord_X.Y.Z.orig.tar.gz, maicord_X.Y.Z-<revision>~<series>1.dsc, .debian.tar.xz and _source.changes into DIR (default
# ./deb-source), unsigned. To publish (owner: needs a Launchpad account, a PPA and the GPG key registered there):
#
#   debsign -k <KEYID> maicord_*_source.changes && dput ppa:<launchpad-user>/maicord maicord_*_source.changes
#
# once per series (a version carries its series in the revision, ~noble1, so the same program can go to several). The
# tarball must be the release's own (its SHA-256 is in the release's .sha256 asset and in the signed manifest). Needs dpkg-dev.
set -eu

tarball=""
series=""
revision=1
output="$PWD/deb-source"
maintainer="${DEBFULLNAME:-}${DEBEMAIL:+ <$DEBEMAIL>}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --tarball) tarball="$2"; shift 2 ;;
        --series) series="$2"; shift 2 ;;
        --revision) revision="$2"; shift 2 ;;
        --output) output="$2"; shift 2 ;;
        --maintainer) maintainer="$2"; shift 2 ;;
        -h|--help) sed -n '2,/^set -eu/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
done
if [ ! -f "$tarball" ] || [ -z "$series" ]; then echo "--tarball and --series are required (see --help)" >&2; exit 2; fi
case "$maintainer" in
    *" <"*"@"*">") ;;
    *) echo "--maintainer 'Name <address>' is required (or DEBFULLNAME and DEBEMAIL): the address of your Launchpad signing key" >&2; exit 2 ;;
esac
here="$(cd "$(dirname "$0")" && pwd -P)"
case "$(basename "$tarball")" in
    maicord-*-linux-x86_64.tar.gz) ;;
    *) echo "Not a MaiCord release tarball: $tarball" >&2; exit 2 ;;
esac
version="$(basename "$tarball" .tar.gz)"
version="${version#maicord-}"
version="${version%-linux-x86_64}"
case "$version" in
    *-dev.*) echo "Only releases go to a PPA, not $version" >&2; exit 2 ;;
esac

mkdir -p "$output"
output="$(cd "$output" && pwd -P)"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
tarball="$(cd "$(dirname "$tarball")" && pwd -P)/$(basename "$tarball")"

# The orig tarball is the release tarball as it is; the source tree is its unpacked directory named maicord-<version>.
cp "$tarball" "$scratch/maicord_$version.orig.tar.gz"
mkdir "$scratch/unpacked"
tar -xzf "$tarball" -C "$scratch/unpacked"
mv "$scratch/unpacked/maicord-$version-linux-x86_64" "$scratch/maicord-$version"
cp -R "$here/debian" "$scratch/maicord-$version/debian"
# A checkout on a Windows or network mount has every file executable, and debhelper runs an executable config as a script.
find "$scratch/maicord-$version/debian" -type f -exec chmod 0644 {} +
chmod 0755 "$scratch/maicord-$version/debian/rules"
sed -i "s|@MAINTAINER@|$maintainer|" "$scratch/maicord-$version/debian/control"
date="$(LC_ALL=C date -u -R)"
cat >"$scratch/maicord-$version/debian/changelog" <<CHANGELOG
maicord ($version-$revision~${series}1) $series; urgency=medium

  * MaiCord $version: https://github.com/maj113/MaiCord/releases/tag/v$version

 -- $maintainer  $date
CHANGELOG
# The upstream tarball's own debian-looking files would clash; the tarball has none.
(cd "$scratch/maicord-$version" && dpkg-buildpackage -S -sa -d -us -uc >"$scratch/build.log" 2>&1) || { cat "$scratch/build.log" >&2; exit 1; }
mv "$scratch"/maicord_* "$output/"
echo "Source package for $series in $output:"
ls -1 "$output"
