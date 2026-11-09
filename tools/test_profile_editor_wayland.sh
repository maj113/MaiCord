#!/usr/bin/env bash
# Settings > Profiles against the fake Discord, through tools/test_user_settings_wayland.sh, twice: with Nitro
# (MAICORD_FAKE_NITRO=1) and without it (Try It Out).
#
#   tools/test_profile_editor_wayland.sh --binary PATH [--out DIR]
#
# With Nitro the run picks the Display Name Font by mouse (8Bit) and the Display Name Effect by keyboard (Tab to the
# field, then Return, Down, Return: Gradient), so each dropdown must open at its field and take the pick; the preview
# card draws the name in that style (screenshot "picked"). It opens the gradient's Start colour picker, drags in its
# colour field, picks the first suggested colour, presses the eyedropper (headless sway has no screenshot portal, so the
# picker says it cannot pick here and stays open), types a hex colour over the field's text, applies and saves:
# PATCH /users/@me carries display_name_font_id 8, display_name_effect_id 2 and the colours [0xEC6B00, the gradient's
# default end 0xFFC490]. On Per-server Profiles it picks the server in the server dropdown and 8Bit again by mouse and
# saves PATCH /guilds/{g}/members/@me. Without Nitro it presses Try It Out and makes the same picks: the preview shows
# them, Shuffle changes them, and Save writes no style. A third run picks the owned nameplate: the preview's member row
# shows it and Save sends its sku. A fourth drives the same picker on a role's Custom colour (Server Settings > Roles).
# Each wtype is a new virtual keyboard, which takes the window's focus for a moment:
# an open menu closes on that, so a keyboard walk through one is a single keys: step; the picker stays open.
# Exit status: 0 when every check holds, 77 without the fake Discord's tools, nonzero otherwise.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-profile-editor-qa}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
probe="$here/test_user_settings_wayland.sh"
mkdir -p "$out"

# Output pixels of the 1280x800 output. With Nitro: the Display Name field, the font field and its menu's 8Bit row, the
# gradient's Start swatch, a drag across the picker's colour field, its first suggested colour, its eyedropper, its hex
# field (after the eyedropper's message moved it up) and Apply button, the unsaved-changes bar's Save button, the
# Per-server Profiles tab, its server dropdown and that menu's one row, its font field and that menu's 8Bit row (the
# menu opens above the field: no room below).
name=580,243
font=690,360
font_8bit=440,594
start=446,598
field_drag=500,300,800,330
suggested=530,525
eyedropper=844,470
hex=600,465
apply=820,605
save=1012,736
server_tab=598,153
server=690,281
server_row=440,323
server_font=690,515
server_font_8bit=440,313
# Without Nitro, after Try It Out (its notice above the fields moves them down): the Try It Out button, the Display Name
# field, the font field and its menu's 8Bit row (above the field) and Shuffle.
try=692,252
try_name=580,390
try_font=690,507
try_font_8bit=440,305
shuffle=446,277

MAICORD_FAKE_NITRO=1 bash "$probe" --binary "$binary" --out "$out/nitro" --page profiles \
    --expect '^200 GET /api/v9/users/[0-9]+/profile' \
    --expect '^body PATCH /users/@me \{"display_name_font_id":8,"display_name_effect_id":2,"display_name_colors":\[15493888,16762000\]\}$' \
    --expect '^body PATCH /guilds/[0-9]+/members/@me .*"display_name_font_id":8' \
    "click:$name" "click:$font" wait:1 shot:font_menu "click:$font_8bit" wait:1 \
    "click:$name" keys:Tab,Tab,Return,Down,Return wait:1 shot:picked \
    "click:$start" wait:1 "drag:$field_drag" wait:1 shot:dragged "click:$suggested" wait:1 "click:$eyedropper" wait:1 \
    shot:eyedropper "click:$hex" keys:End,BackSpace,BackSpace,BackSpace,BackSpace,BackSpace,BackSpace,BackSpace \
    type:ec6b00 wait:1 shot:typed "click:$apply" wait:1 "click:$save" wait:2 \
    shot:saved \
    "click:$server_tab" wait:2 "click:$server" wait:1 shot:server_menu "click:$server_row" wait:1 \
    "click:$server_font" wait:1 "click:$server_font_8bit" wait:1 "click:$save" wait:2 shot:server_saved
status=$?
[ "$status" -eq 0 ] || exit "$status"

MAICORD_FAKE_NITRO= bash "$probe" --binary "$binary" --out "$out/try" --page profiles \
    --expect '^200 GET /api/v9/users/[0-9]+/profile' \
    --refuse 'display_name_font_id|display_name_effect_id|display_name_colors' \
    "click:$try" wait:1 "click:$try_font" wait:1 "click:$try_font_8bit" wait:1 \
    "click:$try_name" keys:Tab,Tab,Return,Down,Return wait:1 shot:try_picked "click:$save" wait:2 \
    "click:$shuffle" wait:1 shot:shuffled
status=$?
[ "$status" -eq 0 ] || exit "$status"

# The owned nameplate (the fake owns one, its still served at media/v1/collectibles-shop/{sku}/static): ten notches
# down, Change Nameplate lists it once the purchases arrive; picking and applying it shows the preview's member row
# with the nameplate behind it (screenshot "row"), and Save sends its sku.
MAICORD_FAKE_NITRO=1 bash "$probe" --binary "$binary" --out "$out/nameplate" --page profiles \
    --expect '^200 GET /api/v9/users/@me/collectibles-purchases' \
    --expect '^200 GET /media/v1/collectibles-shop/[0-9]+/static' \
    --expect '^body PATCH /users/@me \{"nameplate_sku_id":"[0-9]+"\}$' \
    scroll:580,500,10 wait:1 click:486,205 wait:2 shot:nameplates click:492,269 wait:1 click:875,656 wait:2 shot:row \
    "click:$save" wait:2 shot:saved
status=$?
[ "$status" -eq 0 ] || exit "$status"

# Server Settings > Roles > Custom colour: the shared colour picker on a role (the fake's "QA Role"): open the role, the
# Custom colour swatch, drag in the picker's colour field, pick the first suggested colour (the first preset, 0x1ABC9C),
# Apply and Save: PATCH /guilds/{g}/roles/{id} carries exactly that colour.
MAICORD_FAKE_ROLE=1 bash "$here/test_server_settings_wayland.sh" --binary "$binary" --out "$out/roles" --page Roles \
    --expect '^body PATCH /guilds/[0-9]+/roles/[0-9]+ \{"color":1752220\}$' \
    wait:2 click:1079,423 wait:2 click:438,529 wait:1 drag:500,300,800,330 wait:1 click:486,525 wait:1 \
    click:820,597 wait:2 shot:applied click:1073,759 wait:2 shot:saved
status=$?
[ "$status" -eq 0 ] || exit "$status"
echo "profile editor: dropdowns by mouse and keyboard, the colour picker, saves, Try It Out, the nameplate row and the" \
    "Roles colour picker hold (screenshots in $out)"
