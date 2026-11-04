#!/usr/bin/env bash
# Functions for the scripts that need a private D-Bus session bus with the mock desktop services
# (tests/platform/dbus/mock_services.py): source this file, then call
#
#   dbus_test_start_bus DIRECTORY NAME     starts a dbus-daemon whose socket is DIRECTORY/NAME.sock; sets
#                                          DBUS_TEST_ADDRESS to its address. The daemon has a configuration of its own with
#                                          no service activation (nothing of the user's desktop starts) and lets any
#                                          client own any name.
#   dbus_test_start_mock DIRECTORY ADDRESS starts the python mock services on that bus and waits until they are ready.
#   dbus_test_stop                         stops everything started here (call it from the script's EXIT trap).
#   dbus_test_available                    succeeds when dbus-daemon and python3 with the dbus and gi modules exist.
#
# The mock services need python3-dbus and python3-gi (apt install dbus-daemon python3-dbus python3-gi).

dbus_test_pids=""

dbus_test_available() {
    command -v dbus-daemon >/dev/null && command -v python3 >/dev/null && python3 -c 'import dbus, gi' 2>/dev/null
}

dbus_test_start_bus() {
    local directory="$1" name="$2"
    mkdir -p "$directory"
    cat > "$directory/$name.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=$directory/$name.sock</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
    dbus-daemon --config-file="$directory/$name.conf" --nofork --print-address > "$directory/$name.address" \
        2> "$directory/$name.log" &
    dbus_test_pids="$dbus_test_pids $!"
    local _
    for _ in $(seq 1 100); do
        [ -s "$directory/$name.address" ] && break
        sleep 0.05
    done
    if [ ! -s "$directory/$name.address" ]; then
        echo "dbus-daemon $name did not start" >&2
        cat "$directory/$name.log" >&2
        return 1
    fi
    DBUS_TEST_ADDRESS="$(head -n1 "$directory/$name.address" | tr -d '\n')"
}

dbus_test_start_mock() {
    local directory="$1" address="$2" repository
    repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
    DBUS_SESSION_BUS_ADDRESS="$address" python3 "$repository/tests/platform/dbus/mock_services.py" \
        > "$directory/mock.out" 2> "$directory/mock.err" &
    local pid=$!
    dbus_test_pids="$dbus_test_pids $pid"
    local _
    for _ in $(seq 1 100); do
        grep -q '^ready' "$directory/mock.out" 2>/dev/null && return 0
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "the mock services exited" >&2
            cat "$directory/mock.err" >&2
            return 1
        fi
        sleep 0.05
    done
    echo "the mock services did not start" >&2
    return 1
}

dbus_test_stop() {
    local pid
    for pid in $dbus_test_pids; do
        kill "$pid" 2>/dev/null
    done
    dbus_test_pids=""
    return 0
}
