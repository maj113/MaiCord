#!/usr/bin/env bash
# Runs the Wayland window integration tests (tests/platform/window/wayland_window.cpp) against a real compositor and
# takes screenshots.
#
#   tools/test_wayland_window.sh                          headless sway, build and run through ctest
#   tools/test_wayland_window.sh --binary PATH            headless sway, run an already built MaiCordWaylandWindowTests
#   tools/test_wayland_window.sh --compositor nested      sway as a window inside the running compositor (WSLg's Weston)
#   tools/test_wayland_window.sh --compositor current     the running compositor as it is: the lifecycle test only
#   tools/test_wayland_window.sh --scale 1.5              sway output scale (fractional-scale and viewporter path)
#   tools/test_wayland_window.sh --scale 2 --integer      the window ignores fractional-scale and viewporter: the integer buffer-scale path
#   tools/test_wayland_window.sh --out DIR                logs and screenshots (default ~/mc-wayland-test)
#
# Headless and nested sway give the window every protocol MaiCord uses (xdg-decoration, fractional-scale, cursor-shape,
# text-input-v3, ...) that WSLg's Weston lacks, and serve the virtual pointer and keyboard protocols the input test
# drives; grim takes the screenshots, and the test program asserts the WindowEvents the window receives. The
# integration tests are opt-in: without this script (MAICORD_WAYLAND_INTEGRATION=1 at configure time) they are
# registered disabled, and the unit tests (maicord_wayland_input) stay in the normal suite.
#
# Needs sway, swaymsg, grim and python3 on the host (apt install sway grim). Run it in WSL from /mnt/d
# or from the ext4 copy; the build step uses tools/build_linux.sh like everything else.
#
# Exit status: nonzero when a test fails (a skipped test, status 77, is not a failure).

set -uo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
# shellcheck source=tools/dbus_test_bus.sh
source "$repository/tools/dbus_test_bus.sh"
# shellcheck source=tools/sway_test.sh
source "$repository/tools/sway_test.sh"
compositor=headless
binary=""
scale=1
out="${MAICORD_WAYLAND_OUT:-$HOME/mc-wayland-test}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --compositor) compositor="$2"; shift 2 ;;
        --binary) binary="$2"; shift 2 ;;
        --scale) scale="$2"; shift 2 ;;
        --integer) export MAICORD_WAYLAND_NO_FRACTIONAL=1; shift ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
case "$compositor" in headless|nested|current) ;; *) echo "Unknown compositor: $compositor" >&2; exit 2 ;; esac

runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export LANG="${LANG:-C.UTF-8}"
sync="$out/sync"
rm -rf "$out"
mkdir -p "$sync/lifecycle" "$sync/input"
# A machine without a login session (a container, a CI runner) has no /run/user/<uid>: the compositor gets a private one.
if [ ! -d "$runtime" ] || [ ! -w "$runtime" ]; then
    runtime="$out/runtime"
    mkdir -p "$runtime"
    chmod 700 "$runtime"
fi
export XDG_RUNTIME_DIR="$runtime"

sway_pid=""
injector_pid=""
cleanup() {
    [ -n "$injector_pid" ] && kill "$injector_pid" 2>/dev/null
    [ -n "$sway_pid" ] && kill "$sway_pid" 2>/dev/null
    dbus_test_stop
    return 0
}
trap cleanup EXIT

# --- the compositor ----------------------------------------------------------------------------------------------

if [ "$compositor" != current ]; then
    for tool in sway swaymsg grim python3; do
        command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
    done
    config="$out/sway.conf"
    # The output has 1280x720 logical pixels at any scale, so the windows have the same room.
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
    sway_test_start "$config" "$out/sway.log" "$compositor" || exit 1
    echo "sway ($compositor) on $WAYLAND_DISPLAY, scale $scale"
else
    echo "the running compositor: $WAYLAND_DISPLAY"
fi

# --- the driver --------------------------------------------------------------------------------------------------
# The input test injects its own pointer and keys (virtual-pointer and virtual-keyboard on a second connection); what it
# cannot know is where the compositor put its window, so the script reads that from sway and writes it. Screenshots
# are the script's, because only it can reach grim.
#
# The drivers start with the compositor, before the build that precedes the tests (build_linux.sh builds, then runs
# ctest), so a driver waits for its test's first file for as long as a build can take: with a shorter wait the drivers
# gave up while the build still ran, took no screenshot and wrote no answer, and the input and EGL tests then waited
# for them until their own deadlines. A test that is skipped never writes its file; the script ends its drivers.
driver_wait="${MAICORD_DRIVER_WAIT:-1800}"

wait_for() {  # file timeout-seconds
    for _ in $(seq 1 $(( $2 * 10 ))); do
        [ -e "$1" ] && return 0
        sleep 0.1
    done
    return 1
}

# "x y width height": the window content's origin in the layout and the layout's size, from sway's tree.
window_geometry() {
    swaymsg -t get_tree | python3 -c '
import json, sys
tree = json.load(sys.stdin)
def walk(node):
    if node.get("app_id") == "maicord":
        rect, window = node["rect"], node["window_rect"]
        return rect["x"] + window["x"], rect["y"] + window["y"]
    for child in node.get("nodes", []) + node.get("floating_nodes", []):
        found = walk(child)
        if found:
            return found
    return None
origin = walk(tree)
if not origin:
    sys.exit(1)
layout = tree["rect"]
print(origin[0], origin[1], layout["width"], layout["height"])'
}

shot() { grim "$out/$1.png" 2> "$out/grim.log" || echo "grim failed: $(cat "$out/grim.log")"; }

drive_lifecycle() {
    wait_for "$sync/lifecycle/ready" "$driver_wait" || return
    sleep 0.6
    shot "$compositor-lifecycle"
    touch "$sync/lifecycle/injected"
}

# 1 when sway's tree says the maicord window holds an idle inhibitor, else 0.
idle_state() {
    swaymsg -t get_tree | python3 -c '
import json, sys
def walk(node):
    if node.get("app_id") == "maicord":
        return 1 if node.get("inhibit_idle") else 0
    for child in node.get("nodes", []) + node.get("floating_nodes", []):
        found = walk(child)
        if found is not None:
            return found
    return None
print(walk(json.load(sys.stdin)) or 0)'
}

# The input test asks (a file `idle-on`, then `idle-off`) and gets the answer in `<name>.result`.
drive_idle() {
    wait_for "$sync/input/idle-on" "$driver_wait" || return
    idle_state > "$sync/input/idle-on.tmp" && mv "$sync/input/idle-on.tmp" "$sync/input/idle-on.result"
    wait_for "$sync/input/idle-off" 60 || return
    idle_state > "$sync/input/idle-off.tmp" && mv "$sync/input/idle-off.tmp" "$sync/input/idle-off.result"
}

drive_input() {
    wait_for "$sync/input/ready" "$driver_wait" || return
    sleep 0.6
    shot "$compositor-window"
    geometry="$(window_geometry)" || { echo "no maicord window in sway's tree" >&2; return; }
    echo "$geometry" > "$sync/input/origin"
    echo "window origin and layout size: $geometry"
}

if [ "$compositor" != current ]; then
    # Concurrent: ctest runs the cases in an order of its own choosing, and each waits for its own files.
    ( drive_lifecycle & drive_input & drive_idle & wait ) > "$out/driver.log" 2>&1 &
    injector_pid=$!
fi

# --- the desktop settings ----------------------------------------------------------------------------------------

# The lifecycle test also checks that the portal's settings reach the window; it needs a private session bus with the
# mock portal (tools/dbus_test_bus.sh), and skips that stage without one.
if dbus_test_available && dbus_test_start_bus "$out" dbus && dbus_test_start_mock "$out" "$DBUS_TEST_ADDRESS"; then
    export DBUS_SESSION_BUS_ADDRESS="$DBUS_TEST_ADDRESS"
    export MAICORD_DBUS_TEST_ADDRESS="$DBUS_TEST_ADDRESS"
    export MAICORD_DBUS_TEST_BUS=mock
else
    echo "No private D-Bus bus with the mock portal (dbus-daemon, python3-dbus, python3-gi): the settings stage is skipped."
fi

# --- the tests ---------------------------------------------------------------------------------------------------

status=0
if [ -n "$binary" ]; then
    echo "== lifecycle"
    MAICORD_WAYLAND_SYNC_DIR="$sync/lifecycle" "$binary" --lifecycle || { rc=$?; [ "$rc" = 77 ] || status=1; }
    if [ "$compositor" != current ]; then
        echo "== input"
        MAICORD_WAYLAND_SYNC_DIR="$sync/input" "$binary" --input || { rc=$?; [ "$rc" = 77 ] || status=1; }
    fi
else
    names="maicord_wayland_window_lifecycle"
    [ "$compositor" != current ] && names="$names,maicord_wayland_window_input"
    MAICORD_WAYLAND_INTEGRATION=1 MAICORD_WAYLAND_SYNC_DIR="$sync" \
        "$repository/tools/build_linux.sh" --keep-intermediates --test --test-names "$names" || status=1
fi

if [ -n "$injector_pid" ]; then
    kill "$injector_pid" 2>/dev/null
    injector_pid=""
fi
[ -s "$out/driver.log" ] && { echo "== driver"; cat "$out/driver.log"; }
ls "$out"/*.png 2>/dev/null | sed 's/^/screenshot: /'
exit "$status"
