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
#   tools/test_client_wayland.sh --terminate TERM|INT|HUP  end the run with that signal and check for a clean exit (status 0, normal_exit logged)
#
# The profile (data, state, cache and config directories) is private to the run and starts empty: the client shows the
# signed-out screens and never meets an account. grim is the capture on sway; WSLg's Weston has no screencopy.
#
# Reported: first_present (from the log), the window's size in sway's tree, CPU time over the idle window, voluntary context
# switches per second summed over every thread (the wakeups), VmRSS and VmHWM, the anonymous, file and private-dirty
# parts of it (smaps_rollup; with --keep, tools/linux_memory_report.py PID splits the running client by mapping) and the
# executable's size. Exit status:
# nonzero when the client exits early, writes an error line to its log, or the screenshot shows only one colour.
#
# Needs sway, swaymsg, grim and python3 with PIL (apt install sway grim python3-pil).

set -uo pipefail

# shellcheck source=tools/sway_test.sh
source "$(dirname "${BASH_SOURCE[0]}")/sway_test.sh"

compositor=headless
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
scale=1
settle=3
idle=10
gallium=""
out="${MAICORD_CLIENT_QA_OUT:-$HOME/mc-client-qa}"
keep=0
terminate=""

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
        --terminate) terminate="$2"; shift 2 ;;
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
case "$compositor" in headless|nested|current) ;; *) echo "Unknown compositor: $compositor" >&2; exit 2 ;; esac
case "$terminate" in ""|TERM|INT|HUP) ;; *) echo "Unknown signal: $terminate (TERM, INT or HUP)" >&2; exit 2 ;; esac
[ -x "$binary" ] || { echo "No client at $binary (tools/build_linux.sh --keep-intermediates builds it)" >&2; exit 1; }

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
output * bg #ff00ff solid_color
default_border none
xwayland disable
default_floating_border none
focus_follows_mouse no
EOF
    sway_test_start "$out/sway.conf" "$out/sway.log" "$compositor" || exit 1
    echo "sway ($compositor) on $WAYLAND_DISPLAY, scale $scale"
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
# The desktop behind the window is magenta (the sway configuration above) and the window covers the output: any magenta
# pixel is the desktop showing through the window.
through = sum(n for n, c in (colors or []) if c == (255, 0, 255))
print("desktop showing through the window: %d pixels" % through)
sys.exit(0 if count > 8 and through == 0 else 1)
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
    grep -E '^(Pss_Anon|Pss_File|Private_Dirty):' "/proc/$client_pid/smaps_rollup" 2>/dev/null | tr -s ' \t' ' ' | sed 's/^/memory: /'
    printf 'binary: %d bytes\n' "$(stat -c %s "$binary")"
    kill -0 "$client_pid" 2>/dev/null || { echo "the client exited while idle" >&2; status=1; }
fi

# --terminate: the signal ends the run through the event loop (src/app/linux/shutdown_signal.cpp), so the client saves what
# a close saves and leaves with status 0; its log ends with the normal exit.
if [ -n "$terminate" ] && [ "$status" = 0 ]; then
    kill "-$terminate" "$client_pid"
    exited=0
    for _ in $(seq 1 100); do
        kill -0 "$client_pid" 2>/dev/null || { exited=1; break; }
        sleep 0.1
    done
    if [ "$exited" = 1 ]; then
        wait "$client_pid"; code=$?
        client_pid=""
        if [ "$code" = 0 ] && grep -q '\[shutdown\] normal_exit' "$log" 2>/dev/null; then
            echo "SIG$terminate: the client left cleanly (status 0, normal_exit logged)"
        else
            echo "SIG$terminate: the client left with status $code, or without a normal_exit in its log" >&2
            status=1
        fi
    else
        echo "SIG$terminate: the client is still running after 10 s" >&2
        status=1
    fi
fi

if [ -f "$log" ]; then
    grep -E '\[E\]' "$log" | sed 's/^/log error: /' | head -20
    grep -qE '\[E\]' "$log" && status=1
    cp "$log" "$out/maicord.log"
fi
ls "$out"/*.png 2>/dev/null | sed 's/^/screenshot: /'
exit "$status"
