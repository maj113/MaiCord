#!/usr/bin/env bash
# Game mentions against the fake Discord, through tools/test_discord_wayland.sh.
#
#   tools/test_game_mentions_wayland.sh --binary PATH [--out DIR]
#
# #general gets a message mentioning two of the fake's games and an id it does not know (<@$id>): the client asks for
# them in one GET /games?game_ids= batch and draws the known ones as mention chips with their icons, the unknown one as
# "unknown-game" (shot chips). Hovering a chip shows "@game Name" (shot tooltip); clicking Starfall Tactics opens its
# game profile, which asks for the game object (GET /games?game_ids={id}) and lists its details and the friend who
# plays it (MAICORD_FAKE_GAME_FRIEND; shots profile and profile-details). Escape closes it. In the composer, "@game "
# starts the game search token and "sta" searches (GET /games/autocomplete); a click inside the token asks again (the
# cached results show the popup, shot search) and a click on the first row picks it, a chip in the draft (shot
# picked); Return sends it as <@$id>. Every wtype step is a new virtual keyboard whose arrival and removal make sway
# move keyboard focus away and back, and the client closes popups when its window loses keyboard focus (the @ member
# list too): a popup that opens just before that focus change is gone, so the search is re-opened by a pointer click
# and picked by one, never by a key from another wtype. Exit status: 0 when every request holds, 77 without the
# fake Discord's tools.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-game-mentions-qa}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
for tool in unshare sway grim wtype openssl python3; do
    command -v "$tool" >/dev/null || { echo "SKIP: $tool is not installed" >&2; exit 77; }
done
unshare -Urn true 2>/dev/null || { echo "SKIP: no user and network namespaces here" >&2; exit 77; }

# Output pixels of the 1280x800 output: the Starfall Tactics chip in the last message and the composer.
# The tooltip and popups are grabbed without shot:'s output rescale, which takes the client's focus and closes them.
chip=533,709
chip_nudge=534,709
composer=600,769
query=462,769         # inside "sta" of the @game token: the caret there asks for the search again
first_result=405,676  # the popup's first row, Starfall Tactics

rm -rf "$out"
mkdir -p "$(dirname "$out")"
messages="$(mktemp)"
trap 'rm -f "$messages"' EXIT
cat > "$messages" <<'JSON'
[{"channel": "general", "author": "other",
  "content": "Squad up in <@$1402418696126992445> tonight? Or <@$1402418491272986635>, not <@$1234567890123456789>."}]
JSON
MAICORD_FAKE_EXTRA_MESSAGES="$messages" MAICORD_FAKE_GAME_FRIEND=1 \
    "$here/test_discord_wayland.sh" --binary "$binary" --out "$out" \
    wait:3 click:488,355 type:qa@example.invalid click:470,439 type:hunter2 key:Return wait:6 shot:chips \
    "move:$chip" wait:2 "move:$chip_nudge" grab:tooltip "click:$chip" wait:3 shot:profile scroll:640,500,15 wait:1 \
    shot:profile-details key:Escape wait:3 "click:$composer" wait:1 "type:@game " type:sta wait:2 "click:$query" wait:1 \
    grab:search "click:$first_result" wait:1 grab:picked key:Return wait:2 close > "$out.log" 2>&1
status=$?
log="$out/discord-requests.log"
failed=0
[ "$status" -eq 0 ] || { echo "FAIL: the client flow failed ($status); see $out.log" >&2; failed=1; }
for pattern in \
    '^200 GET /api/v9/games\?game_ids=[0-9]+&game_ids=[0-9]+&game_ids=[0-9]+$' \
    '^200 GET /api/v9/games\?game_ids=1402418696126992445$' \
    '^200 GET /api/v9/games/autocomplete\?q=sta&filter_group=default$' \
    "^sent '<@\\\$1402418696126992445>"; do
    grep -Eq -- "$pattern" "$log" 2>/dev/null || { echo "FAIL: no request matches: $pattern" >&2; failed=1; }
done
if [ "$failed" -ne 0 ]; then
    echo "requests:" >&2; grep -v '^ws op' "$log" | tail -40 >&2
    exit 1
fi
echo "game mentions: every expected request was sent (screenshots in $out)"
