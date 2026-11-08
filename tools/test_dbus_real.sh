#!/usr/bin/env bash
# Runs maicord_dbus_real: the settings portal, the portal probes and the Secret Service clients against the real thing,
# xdg-desktop-portal with its GTK backend and gnome-keyring, on a private session bus (not GNOME's own session: no shell,
# no GSettings database, schema defaults only).
#
#   tools/test_dbus_real.sh                       build and run through build_linux.sh
#   tools/test_dbus_real.sh --binary PATH         run an already built MaiCordDbusRealTests
#   tools/test_dbus_real.sh --out DIR             logs, the private home and keyring (default ~/mc-dbus-real)
#
# Needs dbus-daemon, xdg-desktop-portal, xdg-desktop-portal-gtk and gnome-keyring (apt install xdg-desktop-portal
# xdg-desktop-portal-gtk gnome-keyring). HOME and the XDG directories point into --out, so the user's own keyring and
# settings are not touched. Exit status: nonzero when a check fails (77, a skip, is not a failure).

set -uo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
binary=""
out="${MAICORD_DBUS_REAL_OUT:-$HOME/mc-dbus-real}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

for tool in dbus-daemon gnome-keyring-daemon; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
portal=""
for candidate in /usr/libexec/xdg-desktop-portal /usr/lib/xdg-desktop-portal; do
    [ -x "$candidate" ] && portal="$candidate"
done
[ -n "$portal" ] || { echo "Missing xdg-desktop-portal" >&2; exit 1; }

original_runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
rm -rf "$out"
mkdir -p "$out/home" "$out/runtime" "$out/data/keyrings" "$out/config" "$out/cache" "$out/state"
chmod 700 "$out/runtime"
# An unencrypted login keyring that is the default collection: a test session has no PAM to make one, and a password
# prompt cannot be answered here. The Secret Service unlocks it without a prompt.
cat > "$out/data/keyrings/login.keyring" <<EOF
[keyring]
display-name=Login
ctime=0
mtime=0
lock-on-idle=false
lock-after=false
EOF
printf 'login' > "$out/data/keyrings/default"
# Without a portals.conf for this desktop the portal does not offer Settings; ask for the GTK backend.
mkdir -p "$out/config/xdg-desktop-portal"
printf '[preferred]\ndefault=gtk\n' > "$out/config/xdg-desktop-portal/portals.conf"
pids=""
cleanup() {
    for pid in $pids; do kill "$pid" 2>/dev/null; done
    # What the bus activated: the portals and the keyring end with the bus; the documents portal's mount may linger.
    sleep 0.3
    fusermount -u "$out/runtime/doc" 2>/dev/null
    return 0
}
trap cleanup EXIT

# The build below runs in that private home: resolve its toolchain first, so the pinned one is fetched into (or found in)
# the user's own cache rather than into the home --out recreates every run.
if [ -z "$binary" ]; then
    MAICORD_LLVM_BIN="$(bash "$repository/tools/resolve_llvm_toolchain.sh")" || exit 1
    export MAICORD_LLVM_BIN
fi
export HOME="$out/home"
export XDG_RUNTIME_DIR="$out/runtime"
export XDG_DATA_HOME="$out/data"
export XDG_CONFIG_HOME="$out/config"
export XDG_CACHE_HOME="$out/cache"
export XDG_STATE_HOME="$out/state"
export XDG_CURRENT_DESKTOP=GNOME
export GSETTINGS_BACKEND=memory
export LANG="${LANG:-C.UTF-8}"
# The GTK portal backend needs a display: the user's own compositor (WSLg, a running session) when there is one.
display="${WAYLAND_DISPLAY:-}"
case "$display" in
    "") ;;
    /*) ;;
    *) display="$original_runtime/$display" ;;
esac
unset DISPLAY
if [ -n "$display" ]; then export WAYLAND_DISPLAY="$display"; else unset WAYLAND_DISPLAY; fi

# The stock session configuration: the services are activated through the .service files of /usr/share/dbus-1/services.
dbus-daemon --session --nofork --print-address > "$out/address" 2> "$out/bus.log" &
pids="$pids $!"
for _ in $(seq 1 100); do
    [ -s "$out/address" ] && break
    sleep 0.05
done
[ -s "$out/address" ] || { echo "dbus-daemon did not start" >&2; cat "$out/bus.log" >&2; exit 1; }
export DBUS_SESSION_BUS_ADDRESS="$(head -n1 "$out/address" | tr -d '\n')"

echo "display for the GTK backend: ${WAYLAND_DISPLAY:-none}"

export MAICORD_DBUS_REAL=1
export MAICORD_SECRET_SERVICE=auto
status=0
if [ -n "$binary" ]; then
    "$binary" || { rc=$?; [ "$rc" = 77 ] || status=1; }
else
    "$repository/tools/build_linux.sh" --keep-intermediates --test --test-names maicord_dbus_real || status=1
fi

echo "--- portal and keyring logs ($out)"
ls "$out/data/keyrings" 2>/dev/null | sed 's/^/keyring file: /'
exit "$status"
