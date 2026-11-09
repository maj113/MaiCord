#!/usr/bin/env bash
# Runs the PipeWire tests of Linux capture against a private PipeWire daemon:
#   MaiCordCapturePipewireTests      a synthetic dma-buf source (dumb buffers of /dev/dri/card0) streams to the screen
#                                    capture's consumer (tests/platform/capture/pipewire_dmabuf.cpp)
#   MaiCordStreamAudioPipewireTests  stream audio records another application's player and never MaiCord's
#                                    (tests/platform/capture/pipewire_audio.cpp)
#
#   tools/test_capture_pipewire.sh --binary-dir DIR   run the already built tests found in DIR
#   tools/test_capture_pipewire.sh                    build them through build_linux.sh, then run them
#
# The daemon runs a configuration of the script's own (the native protocol, access, metadata, client nodes, the adapter,
# the link factory and a dummy driver: no devices, no session manager) in a temporary XDG_RUNTIME_DIR, so nothing of the
# user's desktop is touched. With no session manager, this script stands in for one for the screen capture test: once
# both nodes exist it links the source's output port to the consumer's input port with pw-link, as wireplumber links the
# portal's node for a real capture (stream audio links itself). Needs pipewire and pipewire-bin (pw-link),
# libpipewire-0.3-0, and for the dma-buf test /dev/dri/card0 writable by this user (vgem in WSL or a VM; the video group).
#
# Exit status: nonzero when a test fails (a skipped test, status 77, is not a failure).

set -uo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
binary_directory=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary-dir) binary_directory="$2"; shift 2 ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

command -v pipewire >/dev/null && command -v pw-link >/dev/null || { echo "Missing pipewire or pw-link" >&2; exit 1; }

if [ -z "$binary_directory" ]; then
    "$repository/tools/build_linux.sh" --test --skip-client --keep-intermediates \
        --test-names maicord_capture_pipewire,maicord_stream_audio_pipewire || exit 1
    binary_directory="${MAICORD_LINUX_BUILD:-$HOME/mc-build}"
fi

runtime="$(mktemp -d)"
daemon=""
cleanup() {
    [ -n "$daemon" ] && kill "$daemon" 2>/dev/null && wait "$daemon" 2>/dev/null
    rm -rf "$runtime"
    return 0
}
trap cleanup EXIT

export XDG_RUNTIME_DIR="$runtime"
unset PIPEWIRE_REMOTE
mkdir -p "$runtime/config"
cat > "$runtime/config/maicord-test.conf" <<'EOF'
context.properties = {
    core.daemon = true
    core.name   = pipewire-0
}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    video.convert.* = videoconvert/libspa-videoconvert
    support.*       = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-access }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-spa-node-factory }
]
context.objects = [
    { factory = spa-node-factory
      args = { factory.name = support.node.driver node.name = Dummy-Driver node.group = pipewire.dummy
               priority.driver = 20000 } }
]
EOF
PIPEWIRE_CONFIG_DIR="$runtime/config" pipewire -c maicord-test.conf >"$runtime/pipewire.log" 2>&1 &
daemon=$!
for _ in $(seq 50); do
    [ -S "$runtime/pipewire-0" ] && break
    sleep 0.1
done
[ -S "$runtime/pipewire-0" ] || { echo "The private PipeWire daemon did not start:" >&2; cat "$runtime/pipewire.log" >&2; exit 1; }

status=0
note() {
    if [ "$1" -ne 0 ] && [ "$1" -ne 77 ]; then status=1; fi
}

MAICORD_PIPEWIRE_TEST=1 "$binary_directory/MaiCordCapturePipewireTests" &
test_process=$!
# The session manager's part: link the two nodes when both have their ports.
linked=0
for _ in $(seq 80); do
    kill -0 "$test_process" 2>/dev/null || break
    output="$(timeout 3 pw-link -o 2>/dev/null | grep -m1 '^maicord-test-producer:' || true)"
    input="$(timeout 3 pw-link -i 2>/dev/null | grep -m1 '^maicord-screen-capture:' || true)"
    if [ -n "$output" ] && [ -n "$input" ] && timeout 3 pw-link "$output" "$input" 2>/dev/null; then
        linked=1
        break
    fi
    sleep 0.1
done
wait "$test_process"
code=$?
note "$code"
if [ "$linked" != 1 ] && [ "$code" -ne 77 ]; then
    echo "The nodes were never linked (pw-link -l):"
    timeout 3 pw-link -l 2>/dev/null
fi

MAICORD_PIPEWIRE_TEST=1 "$binary_directory/MaiCordStreamAudioPipewireTests"
note $?

if [ "$status" -ne 0 ]; then
    echo "--- daemon log"
    cat "$runtime/pipewire.log"
fi
exit "$status"
