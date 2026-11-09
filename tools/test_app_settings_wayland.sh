#!/usr/bin/env bash
# The app-behaviour User Settings pages against the fake Discord, through tools/test_user_settings_wayland.sh: each
# change is checked by the user settings it PATCHes (settings-proto/1, docs.discord.food/resources/user-settings-proto).
#
#   tools/test_app_settings_wayland.sh --binary PATH [--out DIR]
#
# Accessibility shows its preview and sets UI Density to Spacious (appearance.ui_density, 13.12 = COZY 2). Show image
# descriptions (text_and_images.view_image_descriptions, 6.7), Allow playback and usage of /tts command
# (text_and_images.enable_tts_command, 6.16, on by default so the click turns it off) and Show stickers in autocomplete
# results (text_and_images.include_stickers_in_autocomplete, 6.28) each PATCH their field. Developer turns on Developer
# Mode (appearance.developer_mode, 13.2), which shows Application Test Mode, and activates the fake's team application
# through its dialog (the access check, the public record), which puts the notice band across the top of the app.
# Each run's PATCH body
# is decoded and must carry the field at the value; screenshots land in DIR/<page>. Exit status: 0 when every request
# holds, 77 without the fake Discord's tools, nonzero otherwise.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-app-settings-qa}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
probe="$here/test_user_settings_wayland.sh"
mkdir -p "$out"
patch='^200 PATCH /api/v9/users/@me/settings-proto/1$'

# Whether some settings PATCH in LOG carries GROUP.FIELD at VALUE (0 or 1 for a switch, the number for an enum); a
# switch may come plain or in a google.protobuf.BoolValue, whose false is the empty message.
carries() {
    python3 - "$@" <<'EOF'
import base64, json, os, sys

log, group, field, value = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
if not os.path.exists(log):
    sys.exit(1)

def fields(data):
    at = 0
    while at < len(data):
        key = shift = 0
        while True:
            byte = data[at]; at += 1; key |= (byte & 0x7F) << shift; shift += 7
            if byte < 0x80:
                break
        number, kind = key >> 3, key & 7
        if kind == 0:
            item = shift = 0
            while True:
                byte = data[at]; at += 1; item |= (byte & 0x7F) << shift; shift += 7
                if byte < 0x80:
                    break
        elif kind == 2:
            length = shift = 0
            while True:
                byte = data[at]; at += 1; length |= (byte & 0x7F) << shift; shift += 7
                if byte < 0x80:
                    break
            item = data[at:at + length]; at += length
        else:
            raise ValueError(f"wire type {kind}")
        yield number, item

def holds(item):
    if isinstance(item, int):
        return item == value
    wrapped = dict(fields(item))
    return wrapped.get(1, 0) == value

prefix = "body PATCH /users/@me/settings-proto/1 "
for line in open(log, encoding="utf-8"):
    if not line.startswith(prefix):
        continue
    settings = base64.b64decode(json.loads(line[len(prefix):])["settings"])
    for number, item in fields(settings):
        if number == group and isinstance(item, bytes):
            if any(n == field and holds(i) for n, i in fields(item)):
                sys.exit(0)
sys.exit(1)
EOF
}

status=0
# One page in its own client; a machine that cannot run the fake Discord skips the whole run (77). GROUP FIELD VALUE
# name the field its PATCH must carry.
page() {
    local name="$1" group="$2" field="$3" value="$4"
    shift 4
    bash "$probe" --binary "$binary" --out "$out/$name" --expect "$patch" "$@"
    local result=$?
    [ "$result" -eq 77 ] && exit 77
    [ "$result" -eq 0 ] || status=1
    if ! carries "$out/$name/discord-requests.log" "$group" "$field" "$value"; then
        echo "FAIL: $name sent no settings PATCH with $group.$field = $value" >&2
        status=1
    fi
}
# Output pixels of the 1280x800 output: the setting each key opens at, flashed in view.
page density 13 12 2 --page accessibility wait:2 shot:preview click:1018,730 wait:2 shot:spacious
page descriptions 6 7 1 --page accessibility.image_descriptions wait:2 click:1043,726 wait:2 shot:on
page tts 6 16 0 --page notifications.tts_command wait:2 click:1043,717 wait:2 shot:off
page stickers 6 28 1 --page appearance.sticker_autocomplete wait:2 click:1043,732 wait:2 shot:on
# Developer: Developer Mode on, then Application Test Mode with the fake's team application: the access check, the
# public record, and the notice band across the top of the app once Settings closes (key Escape).
page developer 13 2 1 --page advanced.developer_mode \
    --expect '^204 GET /api/v9/activities/1300000000000000001/test-mode$' \
    --expect '^200 GET /api/v9/applications/public\?application_ids=1300000000000000001$' \
    wait:2 click:1043,210 wait:2 shot:developer click:1026,287 wait:1 shot:test_mode type:1300000000000000001 \
    key:Return wait:3 shot:test_mode_active key:Escape wait:2 shot:notice_band
exit "$status"
