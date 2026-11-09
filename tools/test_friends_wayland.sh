#!/usr/bin/env bash
# Home's Friends and Message Requests pages against the fake Discord (tools/test_discord_wayland.sh, headless sway),
# with the fake's other user a friend playing a game (MAICORD_FAKE_GAME_FRIEND) and a stranger's DM waiting as a
# message request (MAICORD_FAKE_MESSAGE_REQUEST).
#
#   tools/test_friends_wayland.sh --binary PATH [--out DIR]
#
# The client signs in on Home's Friends page (shot friends: the title and tabs in the header, the Online list, the
# friend's activity line and Active Now). Add Friend: a username the fake refuses (80000), sent with Enter, then the
# friend's own (80007) with a note, sent with the field's button, each as POST /users/@me/relationships and reported
# under the fields (shots refused, already). The friend's More menu (shot menu) > Add Friend Nickname (shot dialog)
# saves "Bestie" with PATCH /users/@me/relationships/{id} (shot nickname). Message Requests (shot requests) accepts the
# stranger's DM with PUT /channels/{id}/recipients/@me (shot accepted).
# Exit status: 0 when every request was sent, 77 when the machine cannot run the fake Discord, nonzero otherwise.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-friends-qa}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
for tool in unshare sway grim wtype openssl python3; do
    command -v "$tool" >/dev/null || { echo "SKIP: $tool is not installed" >&2; exit 77; }
done
unshare -Urn true 2>/dev/null || { echo "SKIP: no user and network namespaces here" >&2; exit 77; }
rm -rf "$out"
# Output pixels of the 1280x800 output: the header's Online and Add Friend tabs, Add Friend's note field and Send button,
# the friend row's More button, the menu's nickname row, the nickname dialog's field, the sidebar's Message Requests
# row and the request's Accept DM button.
online="click:460,56"
add_friend="click:645,56"
note_field="click:520,216"
send="click:850,168"
more="click:909,188"
nickname_row="click:986,292"
nickname_field="click:640,441"
requests="click:172,194"
accept="click:1223,140"
clear=()
for _ in $(seq 14); do clear+=(key:BackSpace); done
MAICORD_FAKE_GAME_FRIEND=1 MAICORD_FAKE_MESSAGE_REQUEST=1 "$here/test_discord_wayland.sh" --binary "$binary" --out "$out" \
    wait:3 click:488,355 type:qa@example.invalid click:470,439 type:hunter2 key:Return wait:5 \
    click:32,51 wait:2 shot:home click:184,104 wait:2 shot:friends \
    "$add_friend" wait:1 type:closed.account key:Return wait:2 shot:refused \
    "${clear[@]}" type:guide "$note_field" type:Hello "$send" wait:2 shot:already \
    "$online" wait:1 "$more" wait:1 shot:menu "$nickname_row" wait:1 shot:dialog "$nickname_field" type:Bestie \
    key:Return wait:2 shot:nickname \
    "$requests" wait:2 shot:requests "$accept" wait:6 shot:accepted \
    close > "$out.log" 2>&1
status=$?
log="$out/discord-requests.log"
failed=0
[ "$status" -eq 0 ] || { echo "FAIL: the client flow failed ($status); see $out.log" >&2; failed=1; }
for pattern in '^body POST /users/@me/relationships \{"username":"closed\.account","discriminator":null\}$' \
               '^400 POST /api/v9/users/@me/relationships$' \
               '^body POST /users/@me/relationships \{"username":"guide","discriminator":null,"note":"Hello"\}$' \
               '^body PATCH /users/@me/relationships/100000000000000002 \{"nickname":"Bestie"\}$' \
               '^204 PATCH /api/v9/users/@me/relationships/100000000000000002$' \
               '^body PUT /channels/300000000000000077/recipients/@me \{"consent_status":2\}$' \
               '^204 PUT /api/v9/channels/300000000000000077/recipients/@me$'; do
    grep -Eq -- "$pattern" "$log" 2>/dev/null || { echo "FAIL: no request matches: $pattern" >&2; failed=1; }
done
if [ "$failed" -ne 0 ]; then
    echo "requests:" >&2; grep -v '^ws op' "$log" | tail -40 >&2
    exit 1
fi
echo "Friends: Add Friend, the friend nickname and Message Requests sent every expected request (screenshots in $out)"
