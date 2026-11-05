#!/usr/bin/env bash
# Drives the Linux client (MaiCord) through the real AT-SPI infrastructure as a screen reader does: a private session bus,
# at-spi-bus-launcher (which starts the accessibility bus and at-spi2-registryd), the client on headless sway, and an
# external client of libatspi (tests/platform/atspi/atspi_client.py) that walks the signed-out UI, invokes an action, reads
# and sets a field's text and waits for focus and text events.
#
#   tools/test_atspi.sh                       ~/mc-build/MaiCord on headless sway
#   tools/test_atspi.sh --binary PATH         another build of the client
#   tools/test_atspi.sh --out DIR             logs, sockets and the client's profile (default ~/mc-atspi-test)
#   tools/test_atspi.sh --orca                also start Orca on the same buses and check that it speaks the login fields
#
# Accessibility is off when the client starts (org.a11y.Status says so): the script's first check is that the client has not
# connected to the accessibility bus; the client library then turns IsEnabled on, as a desktop's switch or a starting screen
# reader does, and the rest follows. Nothing here touches the user's session: the buses, the runtime directory and the
# profile are private, and GSettings is the in-memory backend.
#
# Needs sway, dbus-daemon, at-spi2-core (at-spi-bus-launcher and at-spi2-registryd), python3 with the gi module and
# gir1.2-atspi-2.0 (apt install sway dbus-daemon at-spi2-core python3-gi gir1.2-atspi-2.0; for --orca also orca
# speech-dispatcher espeak-ng). Exit status: nonzero when a check fails; 77 when a prerequisite is missing.

set -uo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
binary="${MAICORD_CLIENT:-$HOME/mc-build/MaiCord}"
out="${MAICORD_ATSPI_OUT:-$HOME/mc-atspi-test}"
orca=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --orca) orca=1; shift ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

skip() {
    echo "skipped: $*" >&2
    exit 77
}

for tool in sway swaymsg dbus-daemon python3; do
    command -v "$tool" >/dev/null || skip "missing tool $tool"
done
launcher=""
for candidate in /usr/libexec/at-spi-bus-launcher /usr/lib/at-spi2-core/at-spi-bus-launcher /usr/lib/at-spi2/at-spi-bus-launcher \
    /usr/lib/x86_64-linux-gnu/at-spi2-core/at-spi-bus-launcher; do
    [ -x "$candidate" ] && launcher="$candidate" && break
done
[ -n "$launcher" ] || skip "at-spi-bus-launcher is not installed (at-spi2-core)"
python3 -c 'import gi; gi.require_version("Atspi", "2.0"); from gi.repository import Atspi' 2>/dev/null ||
    skip "python3 gi with gir1.2-atspi-2.0 is not installed"
[ -x "$binary" ] || { echo "No client at $binary (tools/build_linux.sh --keep builds it)" >&2; exit 1; }
if [ "$orca" = 1 ]; then
    command -v orca >/dev/null || skip "orca is not installed"
fi

rm -rf "$out"
mkdir -p "$out"
runtime="$out/runtime"
mkdir -p "$runtime"
chmod 700 "$runtime"
export XDG_RUNTIME_DIR="$runtime" LANG="${LANG:-C.UTF-8}" GSETTINGS_BACKEND=memory NO_AT_BRIDGE=0
unset DISPLAY DBUS_SESSION_BUS_ADDRESS AT_SPI_BUS_ADDRESS

pids=()
cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null; done
    # The launcher's own children (the accessibility bus and the registry) end with it.
    pkill -P $$ 2>/dev/null
    return 0
}
trap cleanup EXIT

# The private session bus: no service activation, any client may own any name.
# shellcheck source=tools/dbus_test_bus.sh
source "$repository/tools/dbus_test_bus.sh"
dbus_test_start_bus "$out" session || exit 1
trap 'dbus_test_stop; cleanup' EXIT
export DBUS_SESSION_BUS_ADDRESS="$DBUS_TEST_ADDRESS"

# at-spi-bus-launcher owns org.a11y.Bus on it, starts the accessibility bus daemon and the registry. IsEnabled stays false.
"$launcher" --launch-immediately > "$out/bus-launcher.log" 2>&1 &
pids+=($!)
ready=0
for _ in $(seq 1 100); do
    if dbus-send --session --print-reply --dest=org.a11y.Bus /org/a11y/bus org.a11y.Bus.GetAddress >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 0.1
done
[ "$ready" = 1 ] || { echo "at-spi-bus-launcher did not come up; see $out/bus-launcher.log" >&2; cat "$out/bus-launcher.log" >&2; exit 1; }
# The registry starts with the accessibility bus; give it a moment.
sleep 1

# Headless sway, as tools/test_client_wayland.sh.
cat > "$out/sway.conf" <<EOF
output * resolution 1280x800
default_border none
xwayland disable
default_floating_border none
focus_follows_mouse no
EOF
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
sway -d -c "$out/sway.conf" > "$out/sway.log" 2>&1 &
sway_pid=$!
pids+=("$sway_pid")
display=""
for _ in $(seq 1 100); do
    display="$(sed -n "s/.*Running compositor on wayland display '\\(.*\\)'.*/\\1/p" "$out/sway.log" | tail -n 1)"
    [ -n "$display" ] && [ -S "$runtime/sway-ipc.$(id -u).$sway_pid.sock" ] && break
    display=""
    sleep 0.1
done
[ -n "$display" ] || { echo "sway did not start; see $out/sway.log" >&2; exit 1; }
sleep 0.5
export WAYLAND_DISPLAY="$display" SWAYSOCK="$runtime/sway-ipc.$(id -u).$sway_pid.sock"

profile="$out/profile"
mkdir -p "$profile"
(
    export XDG_DATA_HOME="$profile/data" XDG_STATE_HOME="$profile/state" XDG_CACHE_HOME="$profile/cache" \
        XDG_CONFIG_HOME="$profile/config" MAICORD_SECRET_SERVICE=off
    exec "$binary" > "$out/client.out" 2>&1
) &
client_pid=$!
pids+=("$client_pid")
sleep 4
kill -0 "$client_pid" 2>/dev/null || { echo "the client exited early; see $out/client.out" >&2; cat "$out/client.out" >&2; exit 1; }

status=0
if [ "$orca" = 1 ]; then
    # Orca on the same buses, speaking through speech-dispatcher's dummy module into a file; it starts accessibility itself
    # (ScreenReaderEnabled), so the client library's own switch finds it already on.
    export HOME="$out/home"
    mkdir -p "$HOME/.local/share/orca"
    python3 "$repository/tests/platform/atspi/orca_smoke.py" --out "$out" --client-pid "$client_pid" || status=1
else
    python3 "$repository/tests/platform/atspi/atspi_client.py" 2>&1 | tee "$out/atspi_client.out"
    [ "${PIPESTATUS[0]}" = 0 ] || status=1
fi

kill -0 "$client_pid" 2>/dev/null || { echo "the client exited during the run" >&2; status=1; }
log="$profile/state/maicord/logs/maicord.log"
if [ -f "$log" ]; then
    grep -E '\[E\]' "$log" | sed 's/^/client log error: /' | head -20
    grep -qE '\[E\]' "$log" && status=1
    cp "$log" "$out/maicord.log"
fi
exit "$status"
