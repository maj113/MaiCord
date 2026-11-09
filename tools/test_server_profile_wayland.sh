#!/usr/bin/env bash
# Server Settings > Server Profile against the fake Discord, through tools/test_server_settings_wayland.sh.
#
#   tools/test_server_profile_wayland.sh --binary PATH [--out DIR]
#
# The profile holds three traits, named as the official client writes them: "Chilling" with emoji_name
# "champagne_glass", "Gaming" with "video_game" and "QA" with the server emoji 400000000000000001. The page reads the
# profile (GET /guilds/{g}/profile), the server's most played games (GET /guilds/{g}/top-games) and their game objects
# (GET /games?game_ids=); the server emoji's picture comes from the CDN, and the standard emoji must draw as colour
# pictures on their buttons, not as their names. The run then picks the Cascade banner tile, writes a fourth trait,
# chooses a "Your Members Play" game and saves: PATCH /guilds/{g}/profile carries the preset colour, the stored traits
# with their emoji names, the new trait at its slot and the game. Exit status: 0 when every check holds, 77 without the
# fake Discord's tools, nonzero otherwise.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-server-profile-qa}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
# Output pixels of the 1280x800 output with the page at its top: the Cascade tile (the banner's eighth) and the fourth
# trait's field; at its end (scrolled down): the first "Your Members Play" tile; the unsaved-changes bar's Save
# button; and the "Chilling" trait's emoji button as an x0,y0,x1,y1 box.
cascade=624,516
trait=480,737
suggestion=454,445
save=1109,760
chilling_emoji=412,675,434,696

export MAICORD_FAKE_PROFILE_TRAITS='[{"emoji_name":"champagne_glass","label":"Chilling","position":0},
{"emoji_name":"video_game","label":"Gaming","position":1},
{"emoji_id":"400000000000000001","emoji_name":"qa","label":"QA","position":2}]'
bash "$here/test_server_settings_wayland.sh" --binary "$binary" --out "$out" --page "Server Profile" \
    --expect '^200 GET /api/v9/guilds/[0-9]+/profile$' \
    --expect '^200 GET /api/v9/guilds/[0-9]+/top-games$' \
    --expect '^200 GET /api/v9/games\?game_ids=1158877933042143272&game_ids=1402418491272986635$' \
    --expect '^200 GET /emojis/400000000000000001\.webp' \
    --expect '^body PATCH /guilds/[0-9]+/profile .*"brand_color_primary":"#4fe2ca"' \
    --expect '^body PATCH /guilds/[0-9]+/profile .*\{"label":"Chilling","position":0,"emoji_name":"champagne_glass"\}' \
    --expect '^body PATCH /guilds/[0-9]+/profile .*\{"label":"QA","position":2,"emoji_id":"400000000000000001","emoji_name":"[^"]+","emoji_animated":false\}' \
    --expect '^body PATCH /guilds/[0-9]+/profile .*\{"label":"Chill","position":3\}' \
    --expect '^body PATCH /guilds/[0-9]+/profile .*"game_application_ids":\["1158877933042143272"\]' \
    wait:2 shot:traits "click:$trait" type:Chill "click:$cascade" wait:0.5 scroll:640,500,20 wait:1 \
    "click:$suggestion" wait:1 shot:edited "click:$save" wait:2 shot:saved
status=$?
[ "$status" -eq 0 ] || exit "$status"

# The standard emoji is a colour picture: its button holds saturated pixels (a name drawn as text would be grey).
python3 - "$out/traits.png" "$chilling_emoji" <<'PY'
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

rows, channels = pixels(sys.argv[1])
x0, y0, x1, y1 = (int(v) for v in sys.argv[2].split(","))
coloured = sum(1 for y in range(y0, y1) for x in range(x0, x1)
               if max(rows[y][x * channels:x * channels + 3]) - min(rows[y][x * channels:x * channels + 3]) > 60)
if coloured < 12:
    sys.exit(f"FAIL: the Chilling trait's emoji button has {coloured} colour pixels; the emoji drew as text")
print(f"Chilling's emoji draws as a picture ({coloured} colour pixels)")
PY
