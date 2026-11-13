#!/usr/bin/env bash
# Adds a release's .deb, .rpm and Arch package to the signed apt, dnf and pacman repositories on the Pages site (the
# update-channels branch).
#
#   packaging/repo/publish_site.sh PACKAGES-DIRECTORY SITE-DIRECTORY COMMIT-MESSAGE
#
# Checks the branch out into SITE-DIRECTORY, runs packaging/repo/build_repo.py on its repo/ directory (the older versions
# stay, the newest three are kept), and replaces the branch with one parentless commit of the new site, as
# tools/release/update_channels.ps1 does, so retired packages leave no history. The push is leased on the commit the site
# was built from; a branch that moved is checked out again and the whole update retried. Writes deploy=true|false to
# GITHUB_OUTPUT (deploy=false: the repositories did not change).
#
# Environment: GITHUB_REPOSITORY (owner/name), MAICORD_REPO_SIGNING_KEY (and MAICORD_REPO_SIGNING_PASSPHRASE), and GH_TOKEN
# (contents: write; without it the remote is used unauthenticated: MAICORD_SITE_REMOTE names a local bare repository for a
# dry run).
set -euo pipefail

[ "$#" -eq 3 ] || { sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }
packages="$1"
site="$2"
message="$3"
: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}"
branch=update-channels
owner="${GITHUB_REPOSITORY%%/*}"
name="${GITHUB_REPOSITORY#*/}"
remote="${MAICORD_SITE_REMOTE:-https://github.com/$GITHUB_REPOSITORY.git}"
base_url="https://$(printf '%s' "$owner" | tr '[:upper:]' '[:lower:]').github.io/$name/repo"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

deb="$(find "$packages" -maxdepth 1 -name 'maicord_*_amd64.deb' | sort -V | tail -n 1)"
rpm="$(find "$packages" -maxdepth 1 -name 'maicord-*.x86_64.rpm' | sort -V | tail -n 1)"
pacman_package="$(find "$packages" -maxdepth 1 -name 'maicord-*-x86_64.pkg.tar.zst' | sort -V | tail -n 1)"
if [ -z "$deb" ] || [ -z "$rpm" ] || [ -z "$pacman_package" ]; then echo "No .deb, .rpm or .pkg.tar.zst in $packages" >&2; exit 1; fi

export GIT_AUTHOR_NAME='github-actions[bot]' GIT_COMMITTER_NAME='github-actions[bot]'
export GIT_AUTHOR_EMAIL='41898282+github-actions[bot]@users.noreply.github.com' GIT_COMMITTER_EMAIL='41898282+github-actions[bot]@users.noreply.github.com'
if [ -n "${GH_TOKEN:-}" ]; then
    # Authenticate this process's git commands only; the token never reaches argv.
    auth="$(printf 'x-access-token:%s' "$GH_TOKEN" | base64 -w0)"
    echo "::add-mask::$auth"
    export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0='http.https://github.com/.extraheader' GIT_CONFIG_VALUE_0="AUTHORIZATION: basic $auth"
fi

changed=false
for attempt in 1 2 3; do
    rm -rf "$site"
    mkdir -p "$site"
    git -C "$site" init --quiet --initial-branch "$branch"
    git -C "$site" remote add origin "$remote"
    base=""
    if [ -n "$(git -C "$site" ls-remote --heads origin "refs/heads/$branch")" ]; then
        git -C "$site" fetch --quiet --depth 1 origin "$branch"
        git -C "$site" checkout --quiet -B "$branch" FETCH_HEAD
        base="$(git -C "$site" rev-parse HEAD)"
    fi
    python3 -B "$here/build_repo.py" --site "$site/repo" --base-url "$base_url" --deb "$deb" --rpm "$rpm" --pacman "$pacman_package"
    git -C "$site" add --all repo
    [ -n "$(git -C "$site" status --porcelain -- repo)" ] || break
    tree="$(git -C "$site" write-tree)"
    commit="$(git -C "$site" commit-tree "$tree" -m "$message")"
    if git -C "$site" push --quiet "--force-with-lease=refs/heads/$branch:$base" origin "$commit:refs/heads/$branch"; then
        changed=true
        break
    fi
    [ "$attempt" -lt 3 ] || { echo "Pushing $branch failed 3 times." >&2; exit 1; }
    echo "The $branch branch moved; updating again from its new tip."
done
[ -z "${GITHUB_OUTPUT:-}" ] || echo "deploy=$changed" >>"$GITHUB_OUTPUT"
echo "Package repositories changed: $changed"
