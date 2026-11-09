#!/usr/bin/env bash
# Opens one Server Settings page of the Linux client against the fake Discord (tools/test_discord_wayland.sh, headless
# sway), drives it with tools/client_flow.py steps and checks the requests the page sent.
#
#   tools/test_server_settings_wayland.sh --binary PATH --page LABEL [--out DIR] [--features LIST]
#                                         [--expect REGEX]... [--refuse REGEX]... STEP...
#
# The client signs in (the account owns the fake's QA guild, so every page is open to it) and, as a test build
# (MAICORD_TEST_HOOKS), opens Server Settings on that guild at the page whose sidebar label is LABEL
# (MAICORD_TEST_GUILD_SETTINGS); STEP... then run on it (click:X,Y, type:TEXT, key:NAME, shot:NAME, ...; coordinates
# are output pixels of the 1280x800 output). --features sets the guild's features (MAICORD_FAKE_GUILD_FEATURES: comma
# separated, empty for none; default GUILD_TAGS).
#
# The fake server logs every request as "<status> <method> <path>" and the bodies of the Server Settings writes as
# "body <method> <path> <json>" in DIR/discord-requests.log. Each --expect is an extended regular expression that some
# line of that log must match, each --refuse one that none may match. Screenshots land in DIR. Exit status: 0 when every
# expectation holds, 77 when the machine cannot run the fake Discord (no unshare, sway or grim), nonzero otherwise.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-server-settings-qa}"
page=""
features="GUILD_TAGS"
expects=()
refuses=()
steps=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --page) page="$2"; shift 2 ;;
        --features) features="$2"; shift 2 ;;
        --expect) expects+=("$2"); shift 2 ;;
        --refuse) refuses+=("$2"); shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) steps+=("$1"); shift ;;
    esac
done
[ -n "$page" ] || { echo "--page LABEL is required" >&2; exit 2; }
for tool in unshare sway grim wtype openssl python3; do
    command -v "$tool" >/dev/null || { echo "SKIP: $tool is not installed" >&2; exit 77; }
done
unshare -Urn true 2>/dev/null || { echo "SKIP: no user and network namespaces here" >&2; exit 77; }
rm -rf "$out"
MAICORD_TEST_GUILD_SETTINGS="$page" MAICORD_FAKE_GUILD_FEATURES="$features" \
    "$here/test_discord_wayland.sh" --binary "$binary" --out "$out" \
    wait:3 click:488,355 type:qa@example.invalid click:470,439 type:hunter2 key:Return wait:5 shot:page \
    "${steps[@]+"${steps[@]}"}" close > "$out.log" 2>&1
status=$?
log="$out/discord-requests.log"
failed=0
[ "$status" -eq 0 ] || { echo "FAIL: the client flow failed ($status); see $out.log" >&2; failed=1; }
for pattern in "${expects[@]+"${expects[@]}"}"; do
    grep -Eq -- "$pattern" "$log" 2>/dev/null || { echo "FAIL: no request matches: $pattern" >&2; failed=1; }
done
for pattern in "${refuses[@]+"${refuses[@]}"}"; do
    if grep -Eq -- "$pattern" "$log" 2>/dev/null; then echo "FAIL: a request matches: $pattern" >&2; failed=1; fi
done
if [ "$failed" -ne 0 ]; then
    echo "requests:" >&2; grep -v '^ws op' "$log" | tail -40 >&2
    exit 1
fi
echo "$page: every expected request was sent (screenshots in $out)"
