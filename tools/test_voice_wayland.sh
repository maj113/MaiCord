#!/usr/bin/env bash
# Joins the fake Discord's stage channel with the Linux client and checks that voice works end to end: the voice gateway
# handshake (Identify, Ready, SelectProtocol, Session Description), the IP discovery over UDP, a microphone stream and a
# playback stream open on the sound server, Opus packets that decrypt (AES-256-GCM) at the server, and the packets the server
# sends back as another participant decoded and played (that participant's tile shows the speaking outline).
#
#   tools/test_voice_wayland.sh --binary PATH [--out DIR]
#
# The fake server (tools/fake_discord_server.py, needs python3-cryptography for AES-GCM) runs the voice gateway and a UDP
# socket; tools/test_discord_wayland.sh --audio gives the client a private PulseAudio server with a null sink and a
# speech-like microphone signal (tools/qa_voice_signal.py). The stage is the channel the client may join without DAVE:
# it demands DAVE (MLS) in a voice channel, which the fake server does not speak, and refuses the downgrade there.
#
# Exit status: 0 when every check holds, nonzero otherwise. Needs pulseaudio, pactl, python3 with PIL and cryptography.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-voice-qa}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
python3 -c "import cryptography, PIL" 2>/dev/null || { echo "python3 needs cryptography and PIL" >&2; exit 2; }
rm -rf "$out"
# The stage row is the fourth channel of the list: general, media, Voice, Stage.
"$here/test_discord_wayland.sh" --binary "$binary" --out "$out" --audio \
    wait:3 click:488,355 type:qa@example.invalid click:470,439 type:hunter2 key:Return wait:4 click:130,265 wait:10 \
    audio:capture,playback inhibit shot:stage wait:2 close > "$out.log" 2>&1
status=0
grep -q "^audio capture: [1-9]" "$out.log" && grep -q "^audio playback: [1-9]" "$out.log" ||
    { echo "FAIL: the client has no capture and playback stream on the sound server" >&2; status=1; }
# The call view is open: the window holds an idle inhibitor (the screen stays on in a call).
grep -q "^idle inhibit: 1" "$out.log" || { echo "FAIL: the call view does not hold an idle inhibitor" >&2; status=1; }
requests="$out/discord-requests.log"
for line in "voice select protocol mode aead_aes256_gcm_rtpsize" "voice udp discovery ssrc 4242"; do
    grep -q "$line" "$requests" || { echo "FAIL: no '$line' in the server's log" >&2; status=1; }
done
summary="$(grep "voice summary" "$requests" | tail -1)"
received="$(echo "$summary" | sed -n 's/.*received \([0-9]*\) sent \([0-9]*\) undecryptable \([0-9]*\).*/\1 \3/p')"
if [ -z "$received" ] || [ "${received% *}" -lt 100 ] || [ "${received#* }" -ne 0 ]; then
    echo "FAIL: the Opus stream did not reach the server intact ('$summary')" >&2; status=1
else
    echo "voice: $summary"
fi
python3 - "$out/stage.png" <<'PY' || status=1
import sys
from PIL import Image
image = Image.open(sys.argv[1]).convert("RGB")
for name, point in (("the local tile", (555, 221)), ("the remote participant's tile", (1030, 221))):
    r, g, b = image.getpixel(point)
    if not (g > r + 50 and g > b + 30):
        print(f"FAIL: {name} has no speaking outline ({r}, {g}, {b})", file=sys.stderr)
        sys.exit(1)
print("both stage tiles show the speaking outline")
PY
exit $status
