#!/usr/bin/env bash
# My Account, Data & Privacy and Connected Apps against the fake Discord, through
# tools/test_user_settings_wayland.sh.
#
#   tools/test_account_settings_wayland.sh --binary PATH [--out DIR]
#
# My Account reads the account and its security keys, then changes the username: the dialog's name, then the
# password (hunter2) in its masked field, PATCH /users/@me with both. Data & Privacy reads the consents and the last
# data request, then requests a package of the Account type: POST /users/@me/harvest names it. Connected Apps lists
# the connections and the authorized apps; hiding the connection from the profile PATCHes its visibility. Exit status:
# 0 when every request holds, 77 without the fake Discord's tools, nonzero otherwise.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-account-settings-qa}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
probe="$here/test_user_settings_wayland.sh"
mkdir -p "$out"

# Output pixels of the 1280x800 output: the Username row's Edit button, the Account type and Request Data on Data &
# Privacy, and the connection's "Display on profile" switch on Connected Apps.
username=1037,325
account=1043,696
request=1008,625
display=1043,311

# Each page runs in its own client; a machine that cannot run the fake Discord skips the whole run (77).
status=0
page() {
    local result
    bash "$probe" --binary "$binary" "$@"
    result=$?
    if [ "$result" -eq 77 ]; then exit 77; fi
    if [ "$result" -ne 0 ]; then status=1; fi
}
page --out "$out/account" --page account \
    --expect '^200 GET /api/v9/users/@me$' \
    --expect '^200 GET /api/v9/users/@me/mfa/webauthn/credentials$' \
    --expect '^body PATCH /users/@me \{"username":"qa.two","password":"hunter2"\}$' \
    wait:3 "click:$username" wait:1 keys:BackSpace,BackSpace type:qa.two key:Return wait:1 shot:password \
    type:hunter2 key:Return wait:2 shot:changed
page --out "$out/data" --page data_privacy \
    --expect '^200 GET /api/v9/users/@me/consent$' \
    --expect '^204 GET /api/v9/users/@me/harvest$' \
    --expect '^body POST /users/@me/harvest \{"backends":\["Account"\]\}$' \
    wait:2 "click:$account" wait:1 "click:$request" wait:2 shot:requested
page --out "$out/apps" --page authorized_apps \
    --expect '^200 GET /api/v9/users/@me/connections$' \
    --expect '^200 GET /api/v9/oauth2/tokens$' \
    --expect '^body PATCH /users/@me/connections/steam/[^ ]+ \{"visibility":0\}$' \
    wait:2 "click:$display" wait:2 shot:hidden
exit "$status"
