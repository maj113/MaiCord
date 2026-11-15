#!/usr/bin/env bash
# Runs the media worker against a sound server that stops answering (tests/platform/audio/pulse_stall.cpp): a private
# pulseaudio with a null sink, which the test stops with SIGSTOP and lets go on with SIGCONT. This is how WSLg's
# server stalls while its RDP sink waits for Weston. Nothing reaches the speakers.
#
#   tools/test_pulse_stall.sh                    build and run maicord_pulse_stall_* through ctest
#   tools/test_pulse_stall.sh --binary PATH      run an already built MaiCordPulseStallTests (all three cases)
#
# Needs the pulseaudio package. Exit status: nonzero when a test fails (a skipped test, status 77, is not a failure).

set -uo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
binary=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

command -v pulseaudio >/dev/null || { echo "The pulseaudio package is not installed." >&2; exit 1; }
daemon_directory="$(mktemp -d)"
cleanup() {
    # A test that failed can leave the server stopped: let it go on before it is asked to exit.
    [ -n "${MAICORD_TEST_PULSE_PID:-}" ] && kill -CONT "$MAICORD_TEST_PULSE_PID" 2>/dev/null
    PULSE_RUNTIME_PATH="$daemon_directory" pulseaudio --kill 2>/dev/null
    rm -rf "${daemon_directory:?}"
    return 0
}
trap cleanup EXIT

export XDG_RUNTIME_DIR="$daemon_directory"
export PULSE_RUNTIME_PATH="$daemon_directory"
pulseaudio --daemonize=yes --exit-idle-time=-1 --disallow-exit --fail=yes -n \
    -L "module-native-protocol-unix auth-anonymous=1 socket=$daemon_directory/native" \
    -L "module-null-sink sink_name=maicord_stall" || { echo "pulseaudio did not start" >&2; exit 1; }
export PULSE_SERVER="unix:$daemon_directory/native"
MAICORD_TEST_PULSE_PID="$(cat "$daemon_directory/pid")"
export MAICORD_TEST_PULSE_PID
export MAICORD_PULSE_STALL_INTEGRATION=1
echo "Server: private pulseaudio, process $MAICORD_TEST_PULSE_PID ($PULSE_SERVER)"

if [ -n "$binary" ]; then
    "$binary" --recovers --gives-up --cancel
    status=$?
else
    "$repository/tools/build_linux.sh" --keep-intermediates --test \
        --test-names maicord_pulse_stall_recovers,maicord_pulse_stall_gives_up,maicord_pulse_stall_cancel
    status=$?
fi
[ "$status" = 77 ] && { echo "Skipped."; status=0; }
exit "$status"
