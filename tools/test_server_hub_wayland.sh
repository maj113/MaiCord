#!/usr/bin/env bash
# The Server Hub against the fake Discord, through tools/test_discord_wayland.sh.
#
#   tools/test_server_hub_wayland.sh --binary PATH [--out DIR]
#
# The QA guild has a published hub and a Boost Goal (MAICORD_FAKE_SERVER_HUB=1): its channel list starts with the
# Boost Goal row ("3/33 Boosts") and the Server Hub row (shot list). A click on Server Hub shows the hub in place of
# the chat and the member list, its row selected and the header named "Server Hub". It asks for the space
# (GET /guilds/{id}/space), then for the data of its leaderboard and popular games in one request
# (GET .../space/widgets/hydrate?widget_ids=A&widget_ids=B), and for the consents (GET /users/@me/consent). The
# leaders are not in READY: the client asks the Gateway for them by id (op 8). The hub shows the banner card, the
# leaderboard's list of 20 (scrolling inside its card, the blocked sixth player as "Blocked User"), the account's
# personalization row, the game covers and the text widget (shot hub). The page asks for the server profile
# (GET /guilds/{id}/profile): its brand colour (red) glows in the banner. Hovering a leader's games, the badge and a
# cover shows their tooltips (grabs tip-games, tip-badge, tip-cover). A leader's name opens the profile popout
# (GET /users/{id}/profile, shot profile), which loads and draws the player's red banner (GET /banners/{id}/...); a
# cover opens the game's profile, which loads and draws its purple cover at 160 DIPs (shot game). The row's Open
# Settings opens the Activity Sharing dialog (shot sharing); its personalization switch sends
# POST /users/@me/consent {"grant":["personalization"],"revoke":[]} (shot consent); closed, the account's 15th
# place floats under the list as "QA Tester (You)" (shot hub-consented). The wheel over the list scrolls it, not the
# page (shot hub-end). A click on a channel leaves the hub (shot channel). A second run answers the space with 404
# (MAICORD_FAKE_SERVER_HUB=empty): the hub is empty (shot hub-empty). Exit status: 0 when every request and picture
# holds, 77 without the fake Discord's tools.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-server-hub-qa}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
for tool in unshare sway grim wtype openssl python3; do
    command -v "$tool" >/dev/null || { echo "SKIP: $tool is not installed" >&2; exit 77; }
done
unshare -Urn true 2>/dev/null || { echo "SKIP: no user and network namespaces here" >&2; exit 77; }

# Output pixels of the 1280x800 output: the Server Hub row, under the Boost Goal row at the top of the channel list,
# and the #general row under it.
hub_row="${MAICORD_HUB_ROW:-150,130}"
channel_row="${MAICORD_CHANNEL_ROW:-150,249}"
login=(wait:3 click:488,355 type:qa@example.invalid click:470,439 type:hunter2 key:Return wait:6)

rm -rf "$out" "$out-empty"
mkdir -p "$(dirname "$out")"
MAICORD_FAKE_SERVER_HUB=1 "$here/test_discord_wayland.sh" --binary "$binary" --out "$out" \
    "${login[@]}" shot:list "click:$hub_row" wait:3 shot:hub move:860,640 wait:2 move:861,640 grab:tip-games \
    move:893,376 wait:2 move:894,376 grab:tip-badge move:1002,438 wait:2 move:1003,438 grab:tip-cover \
    click:447,413 wait:2 shot:profile key:Escape wait:1 click:1002,438 wait:3 shot:game key:Escape wait:1 \
    click:852,758 wait:1 shot:sharing click:880,438 wait:2 shot:consent key:Escape wait:2 shot:hub-consented \
    scroll:640,500,15 wait:1 shot:hub-end "click:$channel_row" wait:2 shot:channel close > "$out.log" 2>&1
status=$?
MAICORD_FAKE_SERVER_HUB=empty "$here/test_discord_wayland.sh" --binary "$binary" --out "$out-empty" \
    "${login[@]}" "click:$hub_row" wait:3 shot:hub-empty close > "$out-empty.log" 2>&1
empty_status=$?
log="$out/discord-requests.log"
failed=0
[ "$status" -eq 0 ] || { echo "FAIL: the client flow failed ($status); see $out.log" >&2; failed=1; }
[ "$empty_status" -eq 0 ] ||
    { echo "FAIL: the empty hub flow failed ($empty_status); see $out-empty.log" >&2; failed=1; }
hydrate='^200 GET /api/v9/guilds/[0-9]+/space/widgets/hydrate'
ids='\?widget_ids=700000000000000001&widget_ids=700000000000000002$'
for pattern in \
    '^200 GET /api/v9/guilds/[0-9]+/space$' \
    "$hydrate$ids" \
    '^200 GET /api/v9/users/@me/consent$' \
    '^200 GET /api/v9/users/[0-9]+/profile\?' \
    '^200 GET /api/v9/guilds/[0-9]+/profile$' \
    '^200 GET /banners/[0-9]+/b0a5e0b0a5e0b0a5e0b0a5e0b0a5e0b0\.png' \
    '^200 GET /api/v9/games\?game_ids=[0-9]+$' \
    '^200 GET /app-icons/1402418696126992445/c0ffee00c0ffee00c0ffee00c0ffee00\.png\?size=160' \
    '^body POST /users/@me/consent \{"grant":\["personalization"\],"revoke":\[\]\}$' \
    '^200 POST /api/v9/users/@me/consent$' \
    '^ws op 8 '; do
    grep -Eq -- "$pattern" "$log" 2>/dev/null || { echo "FAIL: no request matches: $pattern" >&2; failed=1; }
done
grep -Eq '^404 GET /api/v9/guilds/[0-9]+/space$' "$out-empty/discord-requests.log" 2>/dev/null &&
    ! grep -q 'space/widgets/hydrate' "$out-empty/discord-requests.log" ||
    { echo "FAIL: the empty hub did not stop at its 404" >&2; failed=1; }
# The pictures drew (uploaded, not just asked for): the hub banner's red glow, the popout's red banner over the
# list, the game profile's purple cover. Output pixels of the 1280x800 output: left,top,right,bottom.
python3 - "$out" <<'PY' || failed=1
import struct, sys, zlib

def pixels(path):
    data = open(path, "rb").read()
    at, idat, width = 8, b"", 0
    while at < len(data):
        size, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + size]
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            channels = {2: 3, 6: 4}[colour]
        elif kind == b"IDAT":
            idat += body
        at += 12 + size
    raw, stride = zlib.decompress(idat), width * channels
    rows, previous = [], bytearray(stride)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b, c = previous[x], previous[x - channels] if x >= channels else 0
            p = a + b - c
            predictor = (a if abs(p - a) <= abs(p - b) and abs(p - a) <= abs(p - c) else
                         b if abs(p - b) <= abs(p - c) else c)
            line[x] = (line[x] + (0, a, b, (a + b) // 2, predictor)[kind]) & 255
        rows.append(line)
        previous = line
    return rows, channels

def count(shot, box, wanted):
    rows, channels = pixels(f"{sys.argv[1]}/{shot}.png")
    x0, y0, x1, y1 = box
    return sum(1 for y in range(y0, y1) for x in range(x0, x1)
               if wanted(rows[y][x * channels:x * channels + 3]))

red = lambda p: p[0] > 150 and p[1] < 80 and p[2] < 90
purple = lambda p: abs(p[0] - 90) < 25 and abs(p[1] - 60) < 25 and abs(p[2] - 160) < 25
checks = [("hub", (340, 110, 1240, 240), red, 20000, "the hub banner's brand colour glow"),
          ("profile", (740, 410, 990, 440), red, 4000, "the popout's banner over the page"),
          ("game", (380, 150, 520, 340), purple, 15000, "the game profile's cover over the page")]
failed = False
for shot, box, wanted, least, name in checks:
    found = count(shot, box, wanted)
    if found < least:
        print(f"FAIL: {name} did not draw ({found} of {least} pixels)", file=sys.stderr)
        failed = True
    else:
        print(f"{name} drew ({found} pixels)")
sys.exit(1 if failed else 0)
PY
if [ "$failed" -ne 0 ]; then
    echo "requests:" >&2; grep -v '^ws op' "$log" | tail -40 >&2
    exit 1
fi
echo "server hub: every expected request was sent (screenshots in $out and $out-empty)"
