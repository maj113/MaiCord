#!/usr/bin/env bash
# Tests the signed pacman repository end to end: builds stand-in packages of four versions (the stand-in program prints its
# version), adds them one release at a time to a repository with build_repo.py under a throwaway key, serves it over HTTP and
# uses it from an `archlinux` container: adds the section and the key, installs, checks the files with `pacman -Qkk`, finds the
# file with `pacman -F`, upgrades, removes, and checks that a changed package, a changed database and a database signed by
# another key are refused. It also compares the database entries with the ones repo-add writes from the same package and
# signature, and checks that the newest three versions stay.
#
#   packaging/repo/test_pacman_repo.sh
#
# Needs: podman (rootless needs `sudo mount --make-rshared /` once; the container runs with seccomp=unconfined), the image
# docker.io/library/archlinux (pulled if absent), network access for the container's mirrors, nfpm on PATH
# (packaging/nfpm/install_nfpm.sh), and what build_repo.py needs (gnupg, apt-utils, createrepo-c, rpm, zstd).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
for tool in podman nfpm gpg python3 zstd apt-ftparchive createrepo_c; do
    command -v "$tool" >/dev/null || { echo "test_pacman_repo: $tool is not on PATH" >&2; exit 2; }
done
scratch="$(mktemp -d)"
port="${MAICORD_TEST_PORT:-18765}"
container="maicord-pacman-test-$$"
server=""
cleanup() {
    [ -z "$server" ] || kill "$server" 2>/dev/null || true
    podman rm -f "$container" >/dev/null 2>&1 || true
    rm -rf "$scratch"
}
trap cleanup EXIT
fail() { echo "test_pacman_repo: $*" >&2; exit 1; }
inside() { podman exec "$container" bash -ec "$1"; }

# Two throwaway keys: the repository's, and one an attacker might sign with.
make_key() {
    mkdir -m 700 "$1"
    GNUPGHOME="$1" gpg --batch --quiet --passphrase '' --quick-generate-key "MaiCord Test $2 <test@example.invalid>" ed25519 sign 1d
}
make_key "$scratch/gnupg" repository
make_key "$scratch/other" attacker
fingerprint="$(GNUPGHOME="$scratch/gnupg" gpg --batch --with-colons --list-keys | awk -F: '/^fpr:/ { print $10; exit }')"
MAICORD_REPO_SIGNING_KEY="$(GNUPGHOME="$scratch/gnupg" gpg --batch --armor --export-secret-keys "$fingerprint")"
export MAICORD_REPO_SIGNING_KEY
unset MAICORD_REPO_SIGNING_PASSPHRASE

site="$scratch/site"
arch_directory="$site/repo/arch/x86_64"
add_release() {
    local version="$1" dist="$scratch/dist/$1"
    python3 -B "$root/packaging/build_tarball.py" --stub --version "$version" --output-dir "$dist" >/dev/null
    sh "$root/packaging/nfpm/build.sh" "$dist/maicord-$version-linux-x86_64.tar.gz" "$dist" >/dev/null
    python3 -B "$root/packaging/repo/build_repo.py" --site "$site/repo" --base-url https://example.invalid/repo \
        --deb "$dist/maicord_${version}_amd64.deb" --rpm "$dist/maicord-$version-1.x86_64.rpm" \
        --pacman "$dist/maicord-$version-1-x86_64.pkg.tar.zst" >/dev/null
}

echo "== build and serve the first release"
add_release 0.6.6
python3 -B -m http.server "$port" --bind 127.0.0.1 --directory "$site" >/dev/null 2>&1 &
server=$!
podman run -d --name "$container" --network host --security-opt seccomp=unconfined \
    -v "$arch_directory:/arch:ro" docker.io/library/archlinux sleep infinity >/dev/null

echo "== add the repository and the key, install"
inside "
pacman-key --init
pacman-key --populate archlinux
printf '[maicord]\nSigLevel = Required\nServer = http://127.0.0.1:$port/repo/arch/\$arch\n' >>/etc/pacman.conf
curl -fsS http://127.0.0.1:$port/repo/maicord.asc | pacman-key --add -
pacman-key --lsign-key $fingerprint
pacman -Syu --noconfirm maicord
[ \"\$(maicord --version)\" = 'MaiCord 0.6.6 (stub)' ]
pacman -Qkk maicord
pacman -Qi maicord
pacman -Fy
pacman -F usr/bin/maicord | grep -q 'maicord/maicord'
"

echo "== upgrade, remove"
add_release 0.6.7
inside "
pacman -Syu --noconfirm
[ \"\$(maicord --version)\" = 'MaiCord 0.6.7 (stub)' ]
pacman -Qkk maicord
pacman -Rn --noconfirm maicord
! pacman -Q maicord
[ ! -e /usr/bin/maicord ] && [ ! -e /usr/lib/maicord ] && [ ! -e /usr/share/applications/io.github.maj113.MaiCord.desktop ]
"

echo "== refusals"
refuse() { # description, command in the container: it must fail, and the package must not be installed
    if inside "$2" >"$scratch/refusal.log" 2>&1; then cat "$scratch/refusal.log"; fail "pacman accepted $1"; fi
    sed 's/^/   /' "$scratch/refusal.log"
    inside '! pacman -Q maicord' || fail "$1 left the package installed"
    echo "refused: $1"
}
# The 0.6.7 package the upgrade downloaded is in pacman's cache, and a cached package is not downloaded again: clear it.
inside 'rm -f /var/cache/pacman/pkg/maicord-*'
package="$arch_directory/maicord-0.6.7-1-x86_64.pkg.tar.zst"
cp "$package" "$scratch/package.saved"
python3 -c "
import sys
data = bytearray(open(sys.argv[1], 'rb').read())
data[len(data) // 2] ^= 1
open(sys.argv[1], 'wb').write(data)" "$package"
refuse 'a changed package' 'pacman -Sy --noconfirm maicord'
cp "$scratch/package.saved" "$package"

cp "$arch_directory/maicord.db" "$scratch/db.saved"
python3 -c "
import gzip, sys
open(sys.argv[1], 'wb').write(gzip.compress(gzip.decompress(open(sys.argv[1], 'rb').read()) + bytes(512), mtime=0))" "$arch_directory/maicord.db"
refuse 'a changed database' 'pacman -Syy --noconfirm maicord'
cp "$scratch/db.saved" "$arch_directory/maicord.db"

GNUPGHOME="$scratch/other" gpg --batch --quiet --yes --detach-sign --output "$arch_directory/maicord.db.sig" "$arch_directory/maicord.db"
refuse 'a database signed by another key' 'pacman -Syy --noconfirm maicord'
cp "$arch_directory/maicord.db.tar.gz.sig" "$arch_directory/maicord.db.sig"

rm "$arch_directory/maicord.db.sig"
refuse 'a database without a signature' 'pacman -Syy --noconfirm maicord'
cp "$arch_directory/maicord.db.tar.gz.sig" "$arch_directory/maicord.db.sig"
inside 'pacman -Syy --noconfirm maicord && pacman -Q maicord && pacman -Rn --noconfirm maicord'

echo "== the newest three versions stay, and the database matches repo-add"
add_release 0.6.8
add_release 0.6.9
listing="$(cd "$arch_directory" && printf '%s\n' maicord-*.pkg.tar.zst | tr '\n' ' ')"
[ "$listing" = 'maicord-0.6.7-1-x86_64.pkg.tar.zst maicord-0.6.8-1-x86_64.pkg.tar.zst maicord-0.6.9-1-x86_64.pkg.tar.zst ' ] ||
    fail "the packages kept are: $listing"
for kept in 0.6.7 0.6.8 0.6.9; do [ -f "$arch_directory/maicord-$kept-1-x86_64.pkg.tar.zst.sig" ] || fail "$kept has no signature"; done
[ ! -e "$arch_directory/maicord-0.6.6-1-x86_64.pkg.tar.zst.sig" ] || fail "the signature of a pruned package stays"
cmp "$arch_directory/maicord.db" "$arch_directory/maicord.db.tar.gz"
cmp "$arch_directory/maicord.files.sig" "$arch_directory/maicord.files.tar.gz.sig"
inside "
mkdir /reference
cp /arch/maicord-0.6.9-1-x86_64.pkg.tar.zst /arch/maicord-0.6.9-1-x86_64.pkg.tar.zst.sig /reference/
cd /reference && repo-add --include-sigs maicord.db.tar.gz maicord-0.6.9-1-x86_64.pkg.tar.zst >/dev/null
[ \"\$(bsdtar -tf /arch/maicord.db.tar.gz | sort)\" = \"\$(bsdtar -tf maicord.db.tar.gz | sort)\" ]
[ \"\$(bsdtar -tf /arch/maicord.files.tar.gz | sort)\" = \"\$(bsdtar -tf maicord.files.tar.gz | sort)\" ]
[ \"\$(bsdtar -xOf /arch/maicord.db.tar.gz maicord-0.6.9-1/desc)\" = \"\$(bsdtar -xOf maicord.db.tar.gz maicord-0.6.9-1/desc)\" ]
for member in desc files; do
    [ \"\$(bsdtar -xOf /arch/maicord.files.tar.gz maicord-0.6.9-1/\$member)\" = \"\$(bsdtar -xOf maicord.files.tar.gz maicord-0.6.9-1/\$member)\" ]
done
pacman -Syu --noconfirm maicord
[ \"\$(maicord --version)\" = 'MaiCord 0.6.9 (stub)' ]
"
echo "the pacman repository tests passed"
