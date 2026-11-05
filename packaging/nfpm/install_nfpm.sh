#!/bin/sh
# Installs the pinned nfpm (https://nfpm.goreleaser.com/) into a directory, checking the release tarball's SHA-256.
#
#   packaging/nfpm/install_nfpm.sh DIRECTORY        puts DIRECTORY/nfpm in place; add DIRECTORY to PATH
#
# The pin is the release's asset digest as GitHub lists it; to move it, take the new version's
# nfpm_<version>_Linux_x86_64.tar.gz and its sha256 from https://github.com/goreleaser/nfpm/releases.
set -eu

version=2.48.0
sha256=6d7f9a28b2f397934403950fcbba83f598b08d02edbe79a0815b3b4897341750

[ "$#" -eq 1 ] || { sed -n '2,/^set -eu/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }
mkdir -p "$1"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
archive="nfpm_${version}_Linux_x86_64.tar.gz"
curl -fsSL --retry 2 -o "$scratch/$archive" "https://github.com/goreleaser/nfpm/releases/download/v$version/$archive"
echo "$sha256  $scratch/$archive" | sha256sum -c - >/dev/null || { echo "$archive does not match the pinned SHA-256" >&2; exit 1; }
tar -xzf "$scratch/$archive" -C "$scratch" nfpm
install -m 0755 "$scratch/nfpm" "$1/nfpm"
"$1/nfpm" --version
