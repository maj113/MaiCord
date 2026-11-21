#!/usr/bin/env bash
# Channels & Roles against the fake Discord, through tools/test_discord_wayland.sh.
#
#   tools/test_channels_roles_wayland.sh --binary PATH [--out DIR]
#
# The QA guild is a community server with onboarding questions (MAICORD_FAKE_GUILD_FEATURES) on a 1700x1000 output, the
# official client's widths for three answers to a line and My Profile beside the questions: its channel list has the
# Channels & Roles row (shot list). A click on it shows the page in place of the chat and the member list, its row
# selected and the header named "Channels & Roles": the Customize tab with the questions of
# GET /guilds/{id}/onboarding, the one with a new answer under New Options, as answer tiles and My Profile (shot
# customize; shot hover with the pointer on an answer). A click on "QA access" picks it: its check badge, the line that
# names the QA Role and its pill in My Profile at once; a second later the client saves with
# PUT /guilds/{id}/onboarding-responses and the NEW badge goes (shot picked). The Browse Channels tab shows the
# introduction and the channels with their boxes (shot browse; shot row with the pointer on #general, which offers
# View); the introduction closes (shot closed). A click on a channel leaves the page (shot channel). Exit status: 0 when
# every request holds, 77 without the fake Discord's tools.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-channels-roles-qa}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
for tool in unshare sway grim wtype openssl python3; do
    command -v "$tool" >/dev/null || { echo "SKIP: $tool is not installed" >&2; exit 77; }
done
unshare -Urn true 2>/dev/null || { echo "SKIP: no user and network namespaces here" >&2; exit 77; }

# Output pixels of the 1700x1000 output: the Channels & Roles row under the Events row at the top of the channel list,
# the "QA access" answer (it gives the QA Role), the Browse Channels tab, #general's row on Browse Channels, the
# introduction's close button and #general in the channel list.
export MAICORD_QA_OUTPUT=1700x1000
roles_row="${MAICORD_ROLES_ROW:-167,140}"
answer="${MAICORD_ROLES_ANSWER:-520,320}"
browse_tab="${MAICORD_ROLES_BROWSE_TAB:-502,126}"
browse_row="${MAICORD_ROLES_BROWSE_ROW:-800,433}"
nux_close="${MAICORD_ROLES_NUX_CLOSE:-1630,212}"
channel_row="${MAICORD_CHANNEL_ROW:-150,215}"
login=(wait:3 click:698,455 type:qa@example.invalid click:680,539 type:hunter2 key:Return wait:6)

rm -rf "$out"
mkdir -p "$(dirname "$out")"
MAICORD_FAKE_GUILD_FEATURES=COMMUNITY,GUILD_ONBOARDING_HAS_PROMPTS MAICORD_FAKE_ROLE=1 \
    "$here/test_discord_wayland.sh" --binary "$binary" --out "$out" \
    "${login[@]}" shot:list "click:$roles_row" wait:3 shot:customize "move:$answer" wait:1 shot:hover \
    "click:$answer" wait:3 shot:picked "click:$browse_tab" wait:2 shot:browse "move:$browse_row" wait:1 shot:row \
    "click:$nux_close" wait:1 shot:closed "click:$channel_row" wait:2 shot:channel close > "$out.log" 2>&1
status=$?
log="$out/discord-requests.log"
failed=0
[ "$status" -eq 0 ] || { echo "FAIL: the client flow failed ($status); see $out.log" >&2; failed=1; }
for pattern in \
    '^200 GET /api/v9/guilds/[0-9]+/onboarding$' \
    '^200 PUT /api/v9/guilds/[0-9]+/onboarding-responses$'; do
    grep -Eq -- "$pattern" "$log" 2>/dev/null || { echo "FAIL: no request matches: $pattern" >&2; failed=1; }
done
if [ "$failed" -ne 0 ]; then
    echo "requests:" >&2; grep -v '^ws op' "$log" | tail -40 >&2
    exit 1
fi
echo "channels & roles: every expected request was sent (screenshots in $out)"
