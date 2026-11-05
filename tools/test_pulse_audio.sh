#!/usr/bin/env bash
# Runs the audio integration tests against a real PulseAudio server: tests/platform/audio/pulse_loopback.cpp and, with it,
# maicord_video_playback_audio (tests/app/media/account_video_playback_linux.cpp --audio: a movie's AAC track through
# the media player's session into the server's default sink).
#
#   tools/test_pulse_audio.sh                    pick a server (below), build and run through ctest
#   tools/test_pulse_audio.sh --binary PATH      pick a server, run an already built MaiCordPulseLoopbackTests
#   tools/test_pulse_audio.sh --private          start a private pulseaudio with a null sink, even where a server exists
#
# The server is, in this order: $PULSE_SERVER as it is set; WSLg's /mnt/wslg/PulseServer; a private `pulseaudio
# --daemonize` in a temporary runtime directory (needs the pulseaudio package). A pipewire-pulse socket works through
# $PULSE_SERVER too. The test loads a null sink of its own (module-null-sink, sink_name=maicord_test_sink), plays
# into it by name and through PULSE_SINK (which this script sets, so the streams that name no device land there as
# well, and nothing reaches the speakers), and records the sink's monitor with the voice capture worker. It asserts
# that samples flow: the tone's level and pitch, the clocks, the counters, the volume and the device events.
# The test is opt-in: without MAICORD_PULSE_INTEGRATION=1 at configure time (this script sets it) ctest lists it
# disabled, and the tests that need no server (maicord_audio_pure, maicord_notification_audio,
# maicord_audio_no_server, maicord_audio_no_library) stay in the normal suite.
#
# Exit status: nonzero when the test fails (a skipped test, status 77, is not a failure).

set -uo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
binary=""
private=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        --private) private=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

daemon_directory=""
cleanup() {
    # A run that was killed leaves its null sinks behind; they are the test's, named maicord_test_*.
    if command -v pactl >/dev/null && [ -n "${PULSE_SERVER:-}" ]; then
        pactl list short modules 2>/dev/null | awk '/module-null-sink/ && /maicord_test_/ { print $1 }' |
            while read -r module; do pactl unload-module "$module" 2>/dev/null; done
    fi
    if [ -n "$daemon_directory" ]; then
        PULSE_RUNTIME_PATH="$daemon_directory" pulseaudio --kill 2>/dev/null
        rm -rf "$daemon_directory"
    fi
    return 0
}
trap cleanup EXIT

if [ "$private" = 0 ] && [ -n "${PULSE_SERVER:-}" ]; then
    echo "Server: PULSE_SERVER=$PULSE_SERVER"
elif [ "$private" = 0 ] && [ -S /mnt/wslg/PulseServer ]; then
    export PULSE_SERVER=unix:/mnt/wslg/PulseServer
    echo "Server: WSLg ($PULSE_SERVER)"
elif command -v pulseaudio >/dev/null; then
    daemon_directory="$(mktemp -d)"
    export XDG_RUNTIME_DIR="$daemon_directory"
    export PULSE_RUNTIME_PATH="$daemon_directory"
    pulseaudio --daemonize=yes --exit-idle-time=-1 --disallow-exit --fail=yes -n \
        -L "module-native-protocol-unix auth-anonymous=1 socket=$daemon_directory/native" \
        -L "module-null-sink sink_name=maicord_default" || { echo "pulseaudio did not start" >&2; exit 1; }
    export PULSE_SERVER="unix:$daemon_directory/native"
    echo "Server: private pulseaudio ($PULSE_SERVER)"
else
    echo "No PulseAudio server: set PULSE_SERVER, run inside WSLg, or install pulseaudio." >&2
    exit 1
fi

export PULSE_SINK=maicord_test_sink
export MAICORD_PULSE_INTEGRATION=1
# The movie playback test (maicord_video_playback_audio) plays into a null sink of its own that this script loads (its
# cleanup unloads every maicord_test_* module), so a server with speakers never plays it.
if command -v pactl >/dev/null; then
    pactl load-module module-null-sink sink_name=maicord_test_playback >/dev/null || echo "Could not load the playback sink." >&2
fi

if [ -n "$binary" ]; then
    "$binary"
    status=$?
else
    "$repository/tools/build_linux.sh" --keep --test maicord_pulse_loopback maicord_video_playback_audio
    status=$?
fi
[ "$status" = 77 ] && { echo "Skipped."; status=0; }
exit "$status"
