#!/usr/bin/env bash
# Checks the tray and the single-instance channel against real implementations, on a private session bus:
#
#   * a StatusNotifierHost and Watcher that is not ours: swaybar's tray in headless sway. MaiCord's item registers with it,
#     swaybar reads the item's properties and draws the IconPixmap (a magenta mark: grim screenshots the bar and the
#     pixels are counted); systemd's busctl, a second independent D-Bus implementation, introspects the item and calls the
#     dbusmenu's GetLayout and GetGroupProperties on it.
#   * `gdbus call` (GLib), a real client of org.freedesktop.Application: Open and Activate reach the first instance, with
#     an activation token.
#
#   tools/test_dbus_tray.sh [--binary PATH] [--out DIR]
#
# Needs sway, swaybar, grim, busctl, gdbus and python3 with PIL (apt install sway grim python3-pil libglib2.0-bin).
# Exit status: nonzero when a check fails.

set -uo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
# shellcheck source=tools/dbus_test_bus.sh
source "$repository/tools/dbus_test_bus.sh"
binary=""
out="${MAICORD_DBUS_TRAY_OUT:-$HOME/mc-dbus-tray}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

for tool in sway swaybar swaymsg grim busctl gdbus python3 dbus-daemon; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
[ -n "$binary" ] || { echo "Pass --binary PATH (MaiCordDbusRealTests)" >&2; exit 2; }

runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR="$runtime"
rm -rf "$out"
mkdir -p "$out/tray" "$out/instance"
status=0
sway_pid=""
tray_pid=""
instance_pid=""
cleanup() {
    for pid in $tray_pid $instance_pid $sway_pid; do [ -n "$pid" ] && kill "$pid" 2>/dev/null; done
    dbus_test_stop
    return 0
}
trap cleanup EXIT
fail() { echo "FAIL $1"; status=1; }

# A bus nobody else owns names on: swaybar becomes the StatusNotifierWatcher.
dbus_test_start_bus "$out" bus || exit 1
export DBUS_SESSION_BUS_ADDRESS="$DBUS_TEST_ADDRESS"
export MAICORD_DBUS_REAL=1

cat > "$out/sway.conf" <<EOF
output * resolution 1280x720
default_border none
xwayland disable
bar {
    position top
    height 32
    tray_output *
    status_command sleep 100000
    colors {
        background #202020
    }
}
EOF
known="$(ls "$runtime" | grep '^wayland-[0-9]*$' | sort | tr '\n' ' ')"
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
unset DISPLAY
sway -c "$out/sway.conf" > "$out/sway.log" 2>&1 &
sway_pid=$!
display=""
for _ in $(seq 1 100); do
    for socket in $(ls "$runtime" | grep '^wayland-[0-9]*$'); do
        case " $known " in *" $socket "*) ;; *) display="$socket" ;; esac
    done
    [ -n "$display" ] && [ -n "$(ls "$runtime" | grep '^sway-ipc\..*\.sock$' | head -n 1)" ] && break
    sleep 0.1
done
[ -n "$display" ] || { echo "sway did not start; see $out/sway.log" >&2; exit 1; }
export WAYLAND_DISPLAY="$display"
export SWAYSOCK="$runtime/$(ls -t "$runtime" | grep '^sway-ipc\..*\.sock$' | head -n 1)"
sleep 1  # swaybar and its watcher

wait_for() {  # file seconds
    for _ in $(seq 1 $(( $2 * 10 ))); do
        [ -e "$1" ] && return 0
        sleep 0.1
    done
    return 1
}

# --- the tray -----------------------------------------------------------------------------------------------------

"$binary" --tray "$out/tray" > "$out/tray.log" 2>&1 &
tray_pid=$!
if wait_for "$out/tray/ready" 20; then
    echo "ok   the item registered with swaybar's watcher"
    sleep 1
    grim "$out/bar.png"
    magenta="$(python3 - "$out/bar.png" <<'EOF'
import sys
from PIL import Image
image = Image.open(sys.argv[1]).convert('RGB')
bar = image.crop((0, 0, image.width, 40))
print(sum(1 for pixel in bar.getdata() if pixel[0] > 230 and pixel[1] < 40 and pixel[2] > 230))
EOF
)"
    if [ "${magenta:-0}" -ge 150 ]; then echo "ok   swaybar draws the icon pixmap ($magenta magenta pixels in the bar)"; else fail "swaybar does not show the icon ($magenta magenta pixels; see $out/bar.png)"; fi

    service="$(busctl --user list --no-pager 2>/dev/null | awk '/org.kde.StatusNotifierItem-/ {print $1; exit}')"
    if [ -n "$service" ]; then
        echo "ok   busctl sees the item's name: $service"
        introspection="$(busctl --user introspect "$service" /StatusNotifierItem org.kde.StatusNotifierItem --no-pager 2>&1)"
        grep -q 'IconPixmap' <<<"$introspection" && echo "ok   busctl introspects the item (IconPixmap listed)" || fail "busctl cannot introspect the item"
        title="$(busctl --user get-property "$service" /StatusNotifierItem org.kde.StatusNotifierItem Title 2>&1)"
        [ "$title" = 's "MaiCord"' ] && echo "ok   busctl reads Title" || fail "Title is: $title"
        tooltip="$(busctl --user get-property "$service" /StatusNotifierItem org.kde.StatusNotifierItem ToolTip 2>&1)"
        echo "$tooltip" | grep -q '"tray check"' && echo "ok   busctl reads the ToolTip struct" || fail "ToolTip is: $tooltip"
        pixmap="$(busctl --user get-property "$service" /StatusNotifierItem org.kde.StatusNotifierItem IconPixmap 2>&1 | cut -c1-40)"
        echo "$pixmap" | grep -q '^a(iiay) 2 32 32 4096' && echo "ok   busctl reads IconPixmap: two sizes, 32x32 first" || fail "IconPixmap starts: $pixmap"
        layout="$(busctl --user call -- "$service" /MenuBar com.canonical.dbusmenu GetLayout iias 0 -1 0 2>&1)"
        for label in "Open MaiCord" "Mute" "Deafen" "Quit MaiCord" separator checkmark; do
            echo "$layout" | grep -q "$label" || fail "the menu layout lacks $label: $(echo "$layout" | cut -c1-200)"
        done
        echo "$layout" | grep -q '"Open MaiCord"' && echo "ok   busctl calls GetLayout and parses the menu"
        busctl --user call "$service" /MenuBar com.canonical.dbusmenu GetGroupProperties aias 2 1 4 0 2>&1 | grep -q 'separator' && echo "ok   busctl calls GetGroupProperties" || fail "GetGroupProperties"
        busctl --user call "$service" /StatusNotifierItem org.kde.StatusNotifierItem Activate ii 10 20 >/dev/null 2>&1 && echo "ok   busctl calls Activate" || fail "Activate"
    else
        fail "the item's bus name is not on the bus"
    fi
else
    fail "the item never registered (see $out/tray.log)"
fi
touch "$out/tray/done"
wait "$tray_pid" 2>/dev/null; tray_pid=""
grep -E '^(PASS|FAIL)|^[a-zA-Z]' "$out/tray.log" | grep -v '^PASS' | sed 's/^/tray: /'
grep -q '^FAIL' "$out/tray.log" && status=1

# --- single instance ----------------------------------------------------------------------------------------------

"$binary" --instance "check$$" "$out/instance" > "$out/instance.log" 2>&1 &
instance_pid=$!
if wait_for "$out/instance/ready" 20; then
    name="io.github.maj113.MaiCord"
    # The profile key makes the name io.github.maj113.MaiCord.p<hash>: find it on the bus.
    service="$(busctl --user list --no-pager 2>/dev/null | awk '/io.github.maj113.MaiCord/ {print $1; exit}')"
    path="/$(echo "$service" | tr '.' '/')"
    if [ -n "$service" ]; then
        echo "ok   the instance owns $service"
        gdbus call --session --dest "$service" --object-path "$path" --method org.freedesktop.Application.Open \
            "['discord://-/invite/gdbus']" "{'activation-token': <'tok-gdbus'>}" >/dev/null 2>&1 \
            && echo "ok   gdbus calls Open" || fail "gdbus Open"
        gdbus call --session --dest "$service" --object-path "$path" --method org.freedesktop.Application.Activate \
            "{}" >/dev/null 2>&1 && echo "ok   gdbus calls Activate" || fail "gdbus Activate"
        sleep 0.5
        if grep -q 'discord://-/invite/gdbus token=tok-gdbus' "$out/instance/requests" && grep -q '^(activate)' "$out/instance/requests"; then
            echo "ok   the instance received both requests, with the activation token"
        else
            fail "the requests were not received: $(cat "$out/instance/requests" 2>&1)"
        fi
        gdbus introspect --session --dest "$service" --object-path "$path" 2>&1 | grep -q 'org.freedesktop.Application' \
            && echo "ok   gdbus introspects org.freedesktop.Application" || fail "gdbus introspect"
    else
        fail "the instance's name is not on the bus"
    fi
else
    fail "the instance did not start (see $out/instance.log)"
fi
touch "$out/instance/done"
wait "$instance_pid" 2>/dev/null; instance_pid=""
grep -q '^FAIL' "$out/instance.log" && status=1

echo "screenshot: $out/bar.png"
exit "$status"
