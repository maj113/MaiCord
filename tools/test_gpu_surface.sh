#!/usr/bin/env bash
# Runs the GPU surface demo (tests/gpu/surface_wayland.cpp) against a real compositor and checks what it put on screen.
#
#   tools/test_gpu_surface.sh                          headless sway, build and run through ctest
#   tools/test_gpu_surface.sh --binary PATH            headless sway, run an already built MaiCordGpuSurfaceWaylandTests
#   tools/test_gpu_surface.sh --compositor nested      sway as a window inside the running compositor (WSLg's Weston)
#   tools/test_gpu_surface.sh --compositor current     the running compositor as it is: the demo runs, nothing is captured
#   tools/test_gpu_surface.sh --scale 1.5              sway output scale (the surface is then 1.5 x the logical size in pixels)
#   tools/test_gpu_surface.sh --out DIR                logs and the screenshot (default ~/mc-gpu-surface)
#
# The demo opens a window, draws the ui_quads scene plus a moving square into the GPU API's surface (an EGL window surface
# fed by an offscreen frame texture), presents with the square's rectangle as the only damage, and paces itself on the
# compositor's frame callbacks. grim takes a screenshot while it runs; this script checks pixels the scene fixes (a solid
# blue quad, a yellow disc, the background, and the white square somewhere on its row) against the colours the shaders
# must produce. The demo is opt-in in ctest (MAICORD_WAYLAND_INTEGRATION=1 at configure time, set below).
#
# On WSLg (no screencopy) run it with --compositor current and look at the window, or capture it from Windows with
# PrintWindow (see the WSL notes); on sway, grim is the capture.
#
# Needs sway, swaymsg, grim and python3 with PIL (apt install sway grim python3-pil). Exit status: nonzero when the demo
# or a pixel check fails (a skip, status 77, is not a failure).

set -uo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
compositor=headless
binary=""
scale=1
hold=4000
out="${MAICORD_GPU_SURFACE_OUT:-$HOME/mc-gpu-surface}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --compositor) compositor="$2"; shift 2 ;;
        --binary) binary="$2"; shift 2 ;;
        --scale) scale="$2"; shift 2 ;;
        --hold) hold="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
case "$compositor" in headless|nested|current) ;; *) echo "Unknown compositor: $compositor" >&2; exit 2 ;; esac

runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export LANG="${LANG:-C.UTF-8}"
rm -rf "$out"
mkdir -p "$out"
# A machine without a login session (a container, a CI runner) has no /run/user/<uid>: the compositor gets a private one.
if [ ! -d "$runtime" ] || [ ! -w "$runtime" ]; then
    runtime="$out/runtime"
    mkdir -p "$runtime"
    chmod 700 "$runtime"
fi
export XDG_RUNTIME_DIR="$runtime"

sway_pid=""
driver_pid=""
cleanup() {
    [ -n "$driver_pid" ] && kill "$driver_pid" 2>/dev/null
    [ -n "$sway_pid" ] && kill "$sway_pid" 2>/dev/null
    return 0
}
trap cleanup EXIT

if [ "$compositor" != current ]; then
    for tool in sway swaymsg grim python3; do
        command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
    done
    config="$out/sway.conf"
    width="$(awk "BEGIN { print int(1280 * $scale) }")"
    height="$(awk "BEGIN { print int(720 * $scale) }")"
    cat > "$config" <<EOF
output * resolution ${width}x${height} scale $scale
default_border none
xwayland disable
default_floating_border none
focus_follows_mouse no
for_window [app_id="maicord"] floating enable, resize set 640 400, move absolute position 100 100
EOF
    if [ "$compositor" = headless ]; then
        export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1
    else
        export WLR_BACKENDS=wayland
    fi
    export WLR_RENDERER=pixman
    unset DISPLAY
    # -d: sway names its Wayland display in the log (other compositors may be starting in the same runtime directory, and a
    # stale socket of an earlier run may have the next name, so the directory cannot tell); its IPC socket is named after
    # its pid.
    sway -d -c "$config" > "$out/sway.log" 2>&1 &
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

# "x y": the window content's origin in the layout, from sway's tree.
window_origin() {
    swaymsg -t get_tree | python3 -c '
import json, sys
def walk(node):
    if node.get("app_id") == "maicord":
        rect, window = node["rect"], node["window_rect"]
        return rect["x"] + window["x"], rect["y"] + window["y"]
    for child in node.get("nodes", []) + node.get("floating_nodes", []):
        found = walk(child)
        if found:
            return found
    return None
origin = walk(json.load(sys.stdin))
if not origin:
    sys.exit(1)
print(origin[0], origin[1])'
}

# Waits for the window, lets a few frames present, takes the screenshot and checks it.
drive() {
    origin=""
    for _ in $(seq 1 100); do
        origin="$(window_origin 2>/dev/null)" && break
        origin=""
        sleep 0.1
    done
    [ -n "$origin" ] || { echo "no maicord window in sway's tree" >&2; return 1; }
    sleep 1.2
    grim "$out/surface.png" 2> "$out/grim.log" || { echo "grim failed: $(cat "$out/grim.log")" >&2; return 1; }
    python3 - "$out/surface.png" "$origin" "$scale" <<'EOF'
import sys
from PIL import Image
path, origin, scale = sys.argv[1], sys.argv[2].split(), float(sys.argv[3])
ox, oy = int(origin[0]), int(origin[1])
image = Image.open(path).convert("RGB")
failures = 0

def pixel(x, y):
    # (x, y) in DIPs of the scene, from the content origin of the layout.
    return image.getpixel((int((ox + x) * scale), int((oy + y) * scale)))

def expect(name, x, y, rgb, tolerance=8):
    global failures
    actual = pixel(x, y)
    ok = all(abs(a - b) <= tolerance for a, b in zip(actual, rgb))
    print("%-34s %s expected %s got %s" % (name, "ok  " if ok else "FAIL", rgb, actual))
    failures += 0 if ok else 1

# The scene's opacity is 0.9 over the clear colour (0.12, 0.13, 0.16): premultiplied, colour * 0.9 + clear * 0.1.
expect("solid blue rounded rectangle", 12, 28, (49, 107, 211))
expect("yellow disc", 132, 24, (221, 187, 50))
expect("background", 380, 300, (31, 33, 41))
# The white square moves along its row; some pixel of the row must show it (opacity 0.9 over the clear colour).
white = 0
for x in range(8, 410):
    r, g, b = pixel(x, 150)
    if r > 215 and g > 215 and b > 215:
        white += 1
print("white pixels on the square's row: %d" % white)
if white < 10:
    print("FAIL the moving square is not on screen")
    failures += 1
sys.exit(1 if failures else 0)
EOF
}

if [ "$compositor" != current ]; then
    drive > "$out/driver.log" 2>&1 &
    driver_pid=$!
fi

status=0
export MAICORD_SURFACE_HOLD_MS="$hold"
if [ -n "$binary" ]; then
    "$binary" || { rc=$?; [ "$rc" = 77 ] || status=1; }
else
    MAICORD_WAYLAND_INTEGRATION=1 "$repository/tools/build_linux.sh" --keep --test maicord_gpu_surface_wayland || status=1
fi

if [ -n "$driver_pid" ]; then
    wait "$driver_pid" || status=1
    driver_pid=""
    cat "$out/driver.log"
fi
ls "$out"/*.png 2>/dev/null | sed 's/^/screenshot: /'
exit "$status"
