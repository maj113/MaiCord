#!/usr/bin/env bash
# Runs the text-to-speech integration test (tests/platform/audio/speechd.cpp --live) against a real speech-dispatcher with its
# dummy output module, and checks in the daemon's log what reached it.
#
#   tools/test_speechd.sh                    start a private daemon, build and run maicord_speechd_live through ctest
#   tools/test_speechd.sh --binary PATH      start a private daemon, run an already built MaiCordSpeechdTests
#
# The daemon is private: a configuration directory, a unix socket and a log of its own in a temporary directory (nothing
# of the user's speech-dispatcher is touched or spoken through; the dummy module makes no sound). The test reaches it
# through $SPEECHD_ADDRESS, as every libspeechd client does. Needs speech-dispatcher (with sd_dummy) and libspeechd.so.2.
#
# Checked in the log: both texts arrive in order from the client "maicord" (the second interrupts the first: a cancel
# comes between them), the text that is not UTF-8 never arrives, the multi-byte text arrives whole, and the client is
# removed when the test closes its connection.
#
# Exit status: nonzero when the test fails or the log lacks what it should hold (a skipped test, status 77, is not a
# failure).

set -uo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
binary=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary) binary="$2"; shift 2 ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

command -v speech-dispatcher >/dev/null || { echo "Missing tool: speech-dispatcher (apt install speech-dispatcher libspeechd2)" >&2; exit 1; }
modules=""
for candidate in /usr/lib/speech-dispatcher-modules /usr/lib64/speech-dispatcher-modules /usr/libexec/speech-dispatcher-modules \
        /usr/lib/x86_64-linux-gnu/speech-dispatcher-modules; do
    [ -x "$candidate/sd_dummy" ] && { modules="$candidate"; break; }
done
[ -n "$modules" ] || { echo "No sd_dummy output module found: it ships in the speech-dispatcher package" >&2; exit 1; }

directory="$(mktemp -d)"
daemon_pid=""
cleanup() {
    [ -n "$daemon_pid" ] && kill "$daemon_pid" 2>/dev/null
    rm -rf "$directory"
    return 0
}
trap cleanup EXIT

mkdir "$directory/config" "$directory/log"
cat > "$directory/config/speechd.conf" <<EOF
LogLevel 5
LogDir "$directory/log"
DefaultVolume 100
AddModule "dummy" "sd_dummy" ""
DefaultModule dummy
EOF
log="$directory/log/speech-dispatcher.log"

speech-dispatcher -s -C "$directory/config" -m "$modules" -c unix_socket -S "$directory/speech.sock" -l 5 -t 0 \
    -P "$directory/speech.pid" > "$directory/daemon.out" 2>&1 &
daemon_pid=$!
for _ in $(seq 1 100); do
    [ -S "$directory/speech.sock" ] && break
    kill -0 "$daemon_pid" 2>/dev/null || { echo "speech-dispatcher exited; see:" >&2; cat "$directory/daemon.out" >&2; exit 1; }
    sleep 0.1
done
[ -S "$directory/speech.sock" ] || { echo "speech-dispatcher did not listen on $directory/speech.sock" >&2; exit 1; }
echo "speech-dispatcher with the dummy module on $directory/speech.sock"

export SPEECHD_ADDRESS="unix_socket:$directory/speech.sock"
status=0
if [ -n "$binary" ]; then
    "$binary" --live
    rc=$?
    [ "$rc" = 77 ] && exit 0
    [ "$rc" = 0 ] || status=1
else
    MAICORD_SPEECHD_INTEGRATION=1 "$repository/tools/build_linux.sh" --keep --test maicord_speechd_live || status=1
fi
[ "$status" = 0 ] || exit "$status"
sleep 0.5

# The daemon logs the data lines it receives ("<fd>:DATA:|<text>"); the client's own name comes with the connection.
expect() {  # description pattern
    if grep -qF -- "$2" "$log"; then
        echo "ok: $1"
    else
        echo "FAIL: $1 (no '$2' in the daemon log $log)" >&2
        status=1
    fi
}
line_of() { grep -nF -- "$1" "$log" | head -n 1 | cut -d: -f1; }

expect "the client is named maicord" "maicord"
expect "the first text arrived" "DATA:|Alice said first message"
expect "the second text arrived" "DATA:|Bob said second message"
expect "the multi-byte text arrived whole" "DATA:|café €"
first="$(line_of 'DATA:|Alice said first message')"
second="$(line_of 'DATA:|Bob said second message')"
if [ -n "$first" ] && [ -n "$second" ] && [ "$first" -lt "$second" ]; then
    echo "ok: the texts arrived in order"
else
    echo "FAIL: the texts did not arrive in order" >&2
    status=1
fi
if grep -q $'DATA:|\xff' "$log"; then
    echo "FAIL: the text that is not UTF-8 reached the daemon" >&2
    status=1
else
    echo "ok: the text that is not UTF-8 never reached the daemon"
fi
cancels="$(grep -cE 'CANCEL' "$log")"
if [ "$cancels" -ge 3 ]; then
    echo "ok: the client cancelled before each text and at release ($cancels cancel lines)"
else
    echo "FAIL: expected cancel requests in the daemon log, found $cancels" >&2
    status=1
fi
expect "the client's connection was closed" "Removing client"
if [ "$status" != 0 ]; then
    echo "-- daemon log excerpt --" >&2
    grep -E "DATA:|CANCEL|Removing client|client" "$log" | head -n 60 >&2
fi
exit "$status"
