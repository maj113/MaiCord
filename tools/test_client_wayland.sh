#!/usr/bin/env bash
# Starts the Linux client (MaiCord) against a real Wayland compositor with a private profile, lets it settle on the signed-out
# login screen, takes a screenshot and reports the idle cost: CPU, wakeups, resident memory and the binary's size.
#
#   tools/test_client_wayland.sh                       headless sway, ~/mc-build/MaiCord
#   tools/test_client_wayland.sh --compositor nested   sway as a window inside the running compositor (WSLg's Weston)
#   tools/test_client_wayland.sh --compositor current  the running compositor as it is (WSLg's Weston: no screenshot)
#   tools/test_client_wayland.sh --binary PATH         another build of the client
#   tools/test_client_wayland.sh --scale 1.5           sway output scale (fractional; 2 is integer)
#   tools/test_client_wayland.sh --gallium d3d12       GALLIUM_DRIVER for the client (WSL's GPU; default llvmpipe's software)
#   tools/test_client_wayland.sh --settle 3 --idle 10  seconds before the first screenshot, and the idle measurement window
#   tools/test_client_wayland.sh --out DIR             logs and screenshots (default ~/mc-client-qa)
#   tools/test_client_wayland.sh --keep                leave the client running when the script ends (sway stays up with it)
#
# The profile (data, state, cache and config directories) is private to the run and starts empty: the client shows the
# signed-out screens and never meets an account. grim is the capture on sway; WSLg's Weston has no screencopy.
#
# Reported: first_present (from the log), the window's size in sway's tree, CPU time over the idle window, voluntary context
# switches per second summed over every thread (the wakeups), VmRSS and VmHWM, and the executable's size. Exit status:
# nonzero when the client exits early, writes an error line to its log, or the screenshot shows only one colour.
#
# Needs sway, swaymsg, grim and python3 with PIL (apt install sway grim python3-pil).

set -uo pipefail

compositor=headless
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
scale=1
settle=3
idle=10
gallium=""
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-client-qa}"
keep=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --compositor) compositor="$2"; shift 2 ;;
        --binary) binary="$2"; shift 2 ;;
        --scale) scale="$2"; shift 2 ;;
        --settle) settle="$2"; shift 2 ;;
        --idle) idle="$2"; shift 2 ;;
        --gallium) gallium="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --keep) keep=1; shift ;;
        -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
case "$compositor" in headless|nested|current) ;; *) echo "Unknown compositor: $compositor" >&2; exit 2 ;; esac
[ -x "$binary" ] || { echo "No client at $binary (tools/build_linux.sh --keep builds it)" >&2; exit 1; }

runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export LANG="${LANG:-C.UTF-8}"
rm -rf "$out"
mkdir -p "$out"
if [ ! -d "$runtime" ] || [ ! -w "$runtime" ]; then
    runtime="$out/runtime"
    mkdir -p "$runtime"
    chmod 700 "$runtime"
fi
export XDG_RUNTIME_DIR="$runtime"

sway_pid=""
client_pid=""
cleanup() {
    if [ "$keep" = 0 ]; then
        [ -n "$client_pid" ] && kill "$client_pid" 2>/dev/null
        [ -n "$sway_pid" ] && kill "$sway_pid" 2>/dev/null
    fi
    return 0
}
trap cleanup EXIT

if [ "$compositor" != current ]; then
    for tool in sway swaymsg grim python3; do
        command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
    done
    width="$(awk "BEGIN { print int(1280 * $scale) }")"
    height="$(awk "BEGIN { print int(800 * $scale) }")"
    cat > "$out/sway.conf" <<EOF
output * resolution ${width}x${height} scale $scale
default_border none
default_floating_border none
focus_follows_mouse no
EOF
    if [ "$compositor" = headless ]; then
        export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1
    else
        export WLR_BACKENDS=wayland
    fi
    export WLR_RENDERER=pixman
    unset DISPLAY
    # -d: sway names its Wayland display in the log (a stale socket of an earlier run may have the next name, so the
    # directory cannot tell); its IPC socket is named after its pid.
    sway -d -c "$out/sway.conf" > "$out/sway.log" 2>&1 &
    sway_pid=$!
    display=""
    for _ in $(seq 1 100); do
        display="$(sed -n "s/.*Running compositor on wayland display '\\(.*\\)'.*/\\1/p" "$out/sway.log" | tail -n 1)"
        [ -n "$display" ] && [ -S "$runtime/sway-ipc.$(id -u).$sway_pid.sock" ] && break
        display=""
        sleep 0.1
    done
    [ -n "$display" ] || { echo "sway did not start; see $out/sway.log" >&2; exit 1; }
    sleep 0.5
    export WAYLAND_DISPLAY="$display"
    export SWAYSOCK="$runtime/sway-ipc.$(id -u).$sway_pid.sock"
    echo "sway ($compositor) on $display, scale $scale"
else
    echo "the running compositor: $WAYLAND_DISPLAY"
fi

profile="$out/profile"
mkdir -p "$profile"
export XDG_DATA_HOME="$profile/data" XDG_STATE_HOME="$profile/state" XDG_CACHE_HOME="$profile/cache" XDG_CONFIG_HOME="$profile/config"
export MAICORD_SECRET_SERVICE=off
[ -n "$gallium" ] && export GALLIUM_DRIVER="$gallium"

"$binary" > "$out/client.out" 2>&1 &
client_pid=$!
status=0
sleep "$settle"
if ! kill -0 "$client_pid" 2>/dev/null; then
    wait "$client_pid"; echo "the client exited early (status $?); see $out/client.out and the log" >&2
    status=1
fi

log="$XDG_STATE_HOME/maicord/logs/maicord.log"
if [ "$status" = 0 ]; then
    if [ "$compositor" != current ]; then
        grim "$out/login.png" 2> "$out/grim.log" || { echo "grim failed: $(cat "$out/grim.log")" >&2; status=1; }
        if [ -f "$out/login.png" ]; then
            python3 - "$out/login.png" <<'EOF' || status=1
import sys
from PIL import Image
image = Image.open(sys.argv[1]).convert("RGB")
colors = image.getcolors(maxcolors=1 << 24)
count = len(colors) if colors else 1 << 24
print("screenshot %dx%d, %d distinct colours" % (image.width, image.height, count))
sys.exit(0 if count > 8 else 1)
EOF
        fi
        swaymsg -t get_tree 2>/dev/null | python3 -c '
import json, sys
def walk(node):
    if node.get("app_id") == "io.github.maj113.MaiCord":
        print("window: app_id %s, %dx%d" % (node["app_id"], node["window_rect"]["width"], node["window_rect"]["height"]))
        return
    for child in node.get("nodes", []) + node.get("floating_nodes", []):
        walk(child)
walk(json.load(sys.stdin))'
    fi
    # The idle cost: CPU and voluntary context switches over `idle` seconds, VmRSS at the end.
    sample() {
        local ticks=0 switches=0 t
        ticks=$(awk '{print $14 + $15}' "/proc/$client_pid/stat")
        for t in /proc/"$client_pid"/task/*/status; do
            switches=$((switches + $(awk '/^voluntary_ctxt_switches/ {print $2}' "$t" 2>/dev/null || echo 0)))
        done
        echo "$ticks $switches"
    }
    read -r ticks_a switches_a < <(sample)
    sleep "$idle"
    read -r ticks_b switches_b < <(sample)
    hertz="$(getconf CLK_TCK)"
    awk -v a="$ticks_a" -v b="$ticks_b" -v s="$idle" -v h="$hertz" 'BEGIN { printf "idle cpu: %.2f%% (%d ticks in %d s)\n", (b - a) / h / s * 100, b - a, s }'
    awk -v a="$switches_a" -v b="$switches_b" -v s="$idle" 'BEGIN { printf "idle wakeups: %.1f per second (%d context switches in %d s)\n", (b - a) / s, b - a, s }'
    grep -E '^(VmRSS|VmHWM)' "/proc/$client_pid/status" | tr -s ' \t' ' ' | sed 's/^/memory: /'
    printf 'binary: %d bytes\n' "$(stat -c %s "$binary")"
    kill -0 "$client_pid" 2>/dev/null || { echo "the client exited while idle" >&2; status=1; }
fi

if [ -f "$log" ]; then
    grep -E '\[E\]' "$log" | sed 's/^/log error: /' | head -20
    grep -qE '\[E\]' "$log" && status=1
    cp "$log" "$out/maicord.log"
fi
ls "$out"/*.png 2>/dev/null | sed 's/^/screenshot: /'
exit "$status"
