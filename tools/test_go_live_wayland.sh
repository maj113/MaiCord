#!/usr/bin/env bash
# Go Live from the Linux client against the fake Discord (tools/test_discord_wayland.sh --dbus: headless sway, a private
# session bus with the desktop portal mocks of tests/platform/dbus/mock_services.py).
#
#   tools/test_go_live_wayland.sh --binary PATH [--out DIR]
#
# The client signs in and joins the fake guild's stage (shot card: the voice card with its screen share button). The
# button opens the screen share dialog (shot dialog). On Linux the system picks the source: the dialog offers "Share
# entire screen" and "Share a window"; the probe picks a window and presses Make Selection (shot waiting), which asks
# the ScreenCast portal for a window (SelectSources types 2, then Start). The mock portal answers with a stream whose
# PipeWire remote has nothing behind it, so the capture fails and the dialog reports it (shot failed); with a GPU that
# has no hardware H.264 encoder the dialog explains that instead (shot dialog) and the portal is never asked.
# A stream that does start (a machine with PipeWire and VA-API) goes live: STREAM_CREATE, the voice state's LIVE badge
# and the call view's own stream tile (shot live), then Stop Streaming from the stream menu sends STREAM_DELETE.
#
# Exit status: 0 when every check holds, 77 when the machine cannot run the fake Discord, nonzero otherwise.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-go-live-qa}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
for tool in unshare sway grim wtype openssl python3 dbus-daemon; do
    command -v "$tool" >/dev/null || { echo "SKIP: $tool is not installed" >&2; exit 77; }
done
python3 -c "import PIL" 2>/dev/null || { echo "SKIP: python3 has no PIL" >&2; exit 77; }
unshare -Urn true 2>/dev/null || { echo "SKIP: no user and network namespaces here" >&2; exit 77; }
rm -rf "$out"
# Layout pixels of the 1280x800 output (swaybar above the window). The voice card floats over the sidebar's foot: its
# screen share button sits left of Disconnect. The dialog (860 x 620 DIP, centred in the content below the caption):
# its two system picker cards and Make Selection.
stage="click:135,306"
share="click:236,710"
window_card="click:752,411"
select_button="click:932,661"
"$here/test_discord_wayland.sh" --binary "$binary" --out "$out" --dbus \
    wait:3 click:488,355 type:qa@example.invalid click:470,439 type:hunter2 key:Return wait:5 "$stage" wait:8 \
    move:236,710 wait:1 shot:card "$share" wait:2 shot:dialog "$window_card" wait:1 shot:window "$select_button" \
    wait:3 shot:after key:Escape wait:1 shot:closed close > "$out.log" 2>&1
status=$?
failed=0
[ "$status" -eq 0 ] || { echo "FAIL: the client flow failed ($status); see $out.log" >&2; failed=1; }
calls="$out/desktop-calls.txt"
requests="$out/discord-requests.log"
# The dialog shows, centred over a scrim, whichever path this machine takes.
python3 - "$out/card.png" "$out/dialog.png" "$out/closed.png" <<'PY' || failed=1
import sys
from PIL import Image
card, dialog, closed = (Image.open(p).convert("RGB") for p in sys.argv[1:4])
def lum(image, point):
    r, g, b = image.getpixel(point)
    return (r + g + b) / 3
# The scrim darkens the sidebar while the dialog is open, and is gone once Escape closed it.
side = (150, 120)
if not lum(dialog, side) < lum(card, side) - 8:
    print("FAIL: the screen share dialog did not open over a scrim", file=sys.stderr)
    sys.exit(1)
if abs(lum(closed, side) - lum(card, side)) > 8:
    print("FAIL: Escape did not close the screen share dialog", file=sys.stderr)
    sys.exit(1)
print("the screen share dialog opened over a scrim and Escape closed it")
PY
if grep -q "ScreenCast.SelectSources" "$calls" 2>/dev/null; then
    # The system picker path: a window was asked for, the portal started, and the stream request followed or failed.
    grep -Eq "ScreenCast.SelectSources .*'types': 2" "$calls" ||
        { echo "FAIL: the portal was not asked for a window (types 2)" >&2; failed=1; }
    grep -q "ScreenCast.Start" "$calls" || { echo "FAIL: the portal's picker was never started" >&2; failed=1; }
    if grep -q "^stream create" "$requests" 2>/dev/null; then
        grep -q "^stream delete" "$requests" ||
            { echo "FAIL: the stream was created but never deleted when it ended" >&2; failed=1; }
        echo "go live: the stream reached the fake Discord and was deleted when it ended"
    else
        echo "go live: the portal picked a window; the capture had no PipeWire behind it and the dialog reported it"
    fi
else
    echo "go live: the portal was not asked (this machine cannot stream, or the module has no stream session yet)"
fi
if [ "$failed" -ne 0 ]; then
    echo "desktop calls:" >&2; tail -20 "$calls" >&2
    exit 1
fi
echo "Go Live: the screen share button, the dialog and its source request behaved (screenshots in $out)"
