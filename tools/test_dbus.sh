#!/usr/bin/env bash
# Runs the D-Bus integration tests (tests/platform/dbus/dbus_bus.cpp, dbus_services.cpp) against private session buses.
#
#   tools/test_dbus.sh                       build and run through build_linux.sh
#   tools/test_dbus.sh --binary-dir DIR      run the already built test programs found in DIR
#   tools/test_dbus.sh --out DIR             logs and sockets (default ~/mc-dbus-test)
#
# Two dbus-daemon instances start (tools/dbus_test_bus.sh: a configuration of their own, no service activation, so
# nothing of the user's desktop is started): one carries tests/platform/dbus/mock_services.py (python3-dbus and
# python3-gi; libdbus is the independent end of every exchange), the other is bare, to test the clients when no service
# is there. The tests read MAICORD_DBUS_TEST_BUS (mock or bare), MAICORD_DBUS_TEST_ADDRESS and DBUS_SESSION_BUS_ADDRESS,
# which must agree; otherwise they skip (77), so they never touch the bus of the user's session.
#
# Needs dbus-daemon and python3 with the dbus and gi modules (apt install dbus-daemon python3-dbus python3-gi).
# Exit status: nonzero when a test fails (a skipped test, status 77, is not a failure).

set -uo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
binary_directory=""
out="${MAICORD_DBUS_OUT:-$HOME/mc-dbus-test}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary-dir) binary_directory="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

# shellcheck source=tools/dbus_test_bus.sh
source "$repository/tools/dbus_test_bus.sh"
dbus_test_available || { echo "Missing dbus-daemon, or python3 with the dbus and gi modules" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$out"
trap dbus_test_stop EXIT

# Two private buses: one with the mock services, one bare.
dbus_test_start_bus "$out" mock || exit 1
mock_address="$DBUS_TEST_ADDRESS"
dbus_test_start_bus "$out" bare || exit 1
bare_address="$DBUS_TEST_ADDRESS"
dbus_test_start_mock "$out" "$mock_address" || exit 1

# The fake xdg-open the OpenURI fallback finds through PATH: it records its arguments.
mkdir -p "$out/bin"
cat > "$out/bin/xdg-open" <<'EOF'
#!/bin/sh
echo "$@" >> "$MAICORD_DBUS_TEST_DIR/xdg-open.log"
EOF
chmod +x "$out/bin/xdg-open"
export PATH="$out/bin:$PATH"
export MAICORD_DBUS_TEST_DIR="$out"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-$out}"
# Tests that keep files under the data directory use the test's own.
export XDG_DATA_HOME="$out/data"
mkdir -p "$XDG_DATA_HOME"

status=0
# One kind of bus per run: $1 names it (mock or bare), $2 is its address, the rest are ctest names (run_ctest) or a
# program of --binary-dir and its arguments (run_program). The tests skip unless these variables agree.
run_ctest() {
    local kind="$1" address="$2"
    shift 2
    DBUS_SESSION_BUS_ADDRESS="$address" MAICORD_DBUS_TEST_ADDRESS="$address" MAICORD_DBUS_TEST_BUS="$kind" \
        "$repository/tools/build_linux.sh" --keep-intermediates --test --test-names "$(IFS=,; echo "$*")" || status=1
}
run_program() {
    local kind="$1" address="$2" program="$3"
    shift 3
    DBUS_SESSION_BUS_ADDRESS="$address" MAICORD_DBUS_TEST_ADDRESS="$address" MAICORD_DBUS_TEST_BUS="$kind" \
        "$binary_directory/$program" "$@"
    local code=$?
    if [ "$code" -ne 0 ] && [ "$code" -ne 77 ]; then status=1; fi
}

if [ -n "$binary_directory" ]; then
    run_program mock "$mock_address" MaiCordDbusWireTests
    run_program mock "$mock_address" MaiCordDbusBusTests
    run_program mock "$mock_address" MaiCordDbusServicesTests
    MAICORD_NOTIFICATION_BACKEND=portal run_program mock "$mock_address" MaiCordDbusServicesTests --portal
    run_program mock "$mock_address" MaiCordDbusServicesTests --plain
    run_program mock "$mock_address" MaiCordLinuxNotificationsTests --mock
    run_program bare "$bare_address" MaiCordDbusServicesTests --bare
    run_program mock "$mock_address" MaiCordSecureBufferServiceTests
    run_program bare "$bare_address" MaiCordSecureBufferServiceTests --bare
    run_program mock "$mock_address" MaiCordDbusDesktopTests
    run_program bare "$bare_address" MaiCordDbusDesktopTests --bare
    run_program bare "$bare_address" MaiCordAtspiBusTests
    MAICORD_INSTANCE_BACKEND=socket run_program mock "$mock_address" MaiCordDbusDesktopTests --socket
else
    run_ctest mock "$mock_address" maicord_dbus_wire maicord_dbus_bus maicord_dbus_services maicord_dbus_secure_buffer \
        maicord_dbus_desktop maicord_dbus_instance_socket maicord_desktop_entry maicord_desktop_entry_validate
    MAICORD_NOTIFICATION_BACKEND=portal run_ctest mock "$mock_address" maicord_dbus_services_portal
    run_ctest mock "$mock_address" maicord_dbus_services_plain
    run_ctest mock "$mock_address" maicord_linux_notifications_mock
    run_ctest bare "$bare_address" maicord_dbus_services_bare maicord_dbus_secure_buffer_bare maicord_dbus_desktop_bare \
        maicord_atspi_bus
fi

echo "--- mock service log ($out/mock.err)"
cat "$out/mock.err"
exit "$status"
