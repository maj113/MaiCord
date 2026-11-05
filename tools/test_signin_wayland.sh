#!/usr/bin/env bash
# Signs the Linux client in against the fake Discord (tools/test_discord_wayland.sh) twice, on headless sway without and
# with a swaybar (which reserves the top 23 pixels of the output), and checks what is on screen.
#
#   tools/test_signin_wayland.sh --binary PATH [--out DIR]
#
# It guards:
#   * a session starts: the Gateway identify must be the Linux client's (GatewayUserRuntime keeps the Linux host fields),
#     or the account worker never starts and the form stays;
#   * the window is painted after the login-to-main transition with no input: the client draws and commits that frame
#     (checked once with a read-back of the default framebuffer), but headless sway scans a window that covers the output
#     out directly and grim then shows the frame from before the commits until the output repaints, so tools/client_flow.py
#     repaints the output before every screenshot; if that regresses, this check fails with the login form on screen;
#   * with a bar the window sits 23 pixels lower and 23 pixels shorter, the rail is still where the layout puts it and a
#     click on the settings cog (output pixels) opens settings.
#
# Exit status: 0 when both runs show what they should, nonzero otherwise. Needs the tools of tools/test_discord_wayland.sh
# and python3 with PIL.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-signin-qa}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
rm -rf "$out" "$out-bar"
"$here/test_discord_wayland.sh" --binary "$binary" --out "$out" \
    wait:3 click:488,355 type:qa@example.invalid click:470,439 type:hunter2 key:Return wait:4 shot:signed-in close > "$out.log" 2>&1
"$here/test_discord_wayland.sh" --binary "$binary" --out "$out-bar" --bar \
    wait:3 click:488,378 type:qa@example.invalid click:470,462 type:hunter2 key:Return wait:4 shot:before click:276,768 wait:2 shot:after close > "$out-bar.log" 2>&1
for run in "$out" "$out-bar"; do
    grep -q "ws op 2" "$run/discord-requests.log" || { echo "FAIL: the client never identified on the gateway ($run)" >&2; tail -20 "$run.log" >&2; exit 1; }
done
python3 - "$out/signed-in.png" "$out-bar/before.png" "$out-bar/after.png" <<'PY' || exit 1
import sys
from PIL import Image, ImageChops, ImageStat
signed_in, before, after = (Image.open(path).convert("RGB") for path in sys.argv[1:4])
for name, image, point in (("without a bar", signed_in, (32, 52)), ("with a bar", before, (32, 75))):
    rail = image.getpixel(point)      # the home button of the guild rail: light in the signed-in window, dark in the form
    if sum(rail) < 600:
        print(f"FAIL: {name}: the guild rail's home button is {rail}: the login form is still on screen", file=sys.stderr)
        sys.exit(1)
    print(f"signed in {name}: guild rail drawn", rail)
box = (130, 100, 370, 700)            # the settings navigation: it covers the channel list once the click opened settings
changed = sum(ImageStat.Stat(ImageChops.difference(before.crop(box), after.crop(box))).mean) / 3
if changed < 3:
    print(f"FAIL: a click on the settings cog with a bar changed the window by {changed:.2f}: settings did not open", file=sys.stderr)
    sys.exit(1)
print(f"with a bar: the settings cog opened settings (mean change {changed:.1f})")
PY
