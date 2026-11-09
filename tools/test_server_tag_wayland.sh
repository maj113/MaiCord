#!/usr/bin/env bash
# Server Settings > Server Tag against the fake Discord, through tools/test_server_settings_wayland.sh.
#
#   tools/test_server_tag_wayland.sh --binary PATH [--out DIR]
#
# Two runs (coordinates are output pixels of the 1280x800 output):
#   * a server with GUILD_TAGS: the page reads GET /guilds/{g}/profile; turning the tag on and typing BUG saves
#     PATCH /guilds/{g}/profile with the tag, the default badge (Leaf, 6) and null colours; then, scrolled down, the
#     Heart badge and the teal pair save {"badge":7,"badge_color_primary":"#32a070","badge_color_secondary":"#57b59e"},
#     and Adopt Tag sends PUT /users/@me/clan {"identity_guild_id":"<g>","identity_enabled":true};
#   * a server without it: the page reads the profile, shows the locked card, and writes nothing.
# Exit status: 0 when both hold, 77 without the fake Discord's tools, nonzero otherwise.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-server-tag-qa}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
probe="$here/test_server_settings_wayland.sh"
profile='^200 GET /api/v9/guilds/[0-9]+/profile$'
patch='^body PATCH /guilds/[0-9]+/profile '

# Enable (the switch), type the tag, Save; scroll to the end; Heart, the teal pair, Save; Adopt Tag.
bash "$probe" --binary "$binary" --out "$out" --page "Server Tag" --expect "$profile" \
    --expect "${patch}\\{\"tag\":\"BUG\",\"badge\":6,\"badge_color_primary\":null,\"badge_color_secondary\":null\\}$" \
    --expect "${patch}\\{\"tag\":\"BUG\",\"badge\":7,\"badge_color_primary\":\"#32a070\",\"badge_color_secondary\":\"#57b59e\"\\}$" \
    --expect '^body PUT /users/@me/clan \{"identity_guild_id":"[0-9]+","identity_enabled":true\}$' \
    --expect '^200 PUT /api/v9/users/@me/clan$' \
    click:1119,238 wait:1 click:773,640 type:BUG wait:1 click:1073,760 wait:2 scroll:773,500,30 wait:1 shot:saved \
    click:531,359 click:583,526 wait:1 click:1073,760 wait:2 click:1097,706 wait:2 shot:adopted
status=$?
[ "$status" -eq 0 ] || exit "$status"

bash "$probe" --binary "$binary" --out "$out-locked" --page "Server Tag" --features "" --expect "$profile" \
    --refuse '^body ' shot:locked
