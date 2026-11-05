#!/usr/bin/env bash
# Runs the opt-in integration scripts of the Linux port, in order, against the build that
# tools/build_linux.sh --test --keep left, and reports each one.
#
#   tools/test_linux_integration.sh                     every case, build in ~/mc-build, logs in ~/mc-integration
#   tools/test_linux_integration.sh wayland atspi       only these cases
#   tools/test_linux_integration.sh --build DIR         another build directory (MAICORD_LINUX_BUILD)
#   tools/test_linux_integration.sh --out DIR           logs and screenshots, one directory per case
#   tools/test_linux_integration.sh --blocking          fail when a case fails
#
# Cases, with the script that runs each (every one needs the tools its header names; tools/install_linux_toolchain.sh
# --integration installs them on Debian and Ubuntu):
#   wayland      tools/test_wayland_window.sh, twice: scale 1 and fractional scale 1.5 (headless sway)
#   gpu-surface  tools/test_gpu_surface.sh (headless sway, llvmpipe)
#   client       tools/test_client_wayland.sh: the client on its signed-out screen (screenshot, idle CPU, wakeups, memory)
#   atspi        tools/test_atspi.sh: the client behind the real at-spi-bus-launcher and registry
#   dbus         tools/test_dbus.sh: the mock desktop services on private buses
#   audio        tools/test_pulse_audio.sh --private: a private PulseAudio server
#   speech       tools/test_speechd.sh: a private speech-dispatcher with its dummy module
#
# A case that fails is reported and does not stop the others. The exit status is 0 unless --blocking (or
# MAICORD_LINUX_INTEGRATION_BLOCKING=true, the repository variable of the CI workflow) is given and a case failed: until the
# scripts have proven stable on hosted runners the CI run only reports them. Under GitHub Actions each case is a log group and
# a failed one an annotation; the table is also written to the job summary.
#
# Not here: tools/test_dbus_real.sh (the real xdg-desktop-portal and gnome-keyring need a graphical session and FUSE) and the
# fake Discord runs (tools/test_discord_wayland.sh, test_signin_wayland.sh, test_voice_wayland.sh, test_catalog_wayland.sh:
# they need a user and network namespace and a built client with the catalog test key).

set -uo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build="${MAICORD_LINUX_BUILD:-$HOME/mc-build}"
out="${HOME}/mc-integration"
blocking=0
[ "${MAICORD_LINUX_INTEGRATION_BLOCKING:-}" = true ] && blocking=1
all=(wayland gpu-surface client atspi dbus audio speech)
selected=()

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build) build="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --blocking) blocking=1; shift ;;
        -h|--help) sed -n '2,/^# Not here/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "Unknown argument: $1" >&2; exit 2 ;;
        *) selected+=("$1"); shift ;;
    esac
done
[ "${#selected[@]}" -gt 0 ] || selected=("${all[@]}")

# One case: the script and its arguments after the case's own directory under --out.
run_case() {
    local name="$1" directory="$out/$1"
    mkdir -p "$directory"
    case "$name" in
        wayland)
            bash "$repository/tools/test_wayland_window.sh" --binary "$build/MaiCordWaylandWindowTests" --out "$directory/scale-1" &&
            bash "$repository/tools/test_wayland_window.sh" --binary "$build/MaiCordWaylandWindowTests" --scale 1.5 --out "$directory/scale-1.5" ;;
        gpu-surface) bash "$repository/tools/test_gpu_surface.sh" --binary "$build/MaiCordGpuSurfaceWaylandTests" --out "$directory" ;;
        client) bash "$repository/tools/test_client_wayland.sh" --binary "$build/MaiCord" --out "$directory" ;;
        atspi) bash "$repository/tools/test_atspi.sh" --binary "$build/MaiCord" --out "$directory" ;;
        dbus) bash "$repository/tools/test_dbus.sh" --binary-dir "$build" --out "$directory" ;;
        audio) bash "$repository/tools/test_pulse_audio.sh" --private --binary "$build/MaiCordPulseLoopbackTests" ;;
        speech) bash "$repository/tools/test_speechd.sh" --binary "$build/MaiCordSpeechdTests" ;;
        *) echo "Unknown case: $name (cases: ${all[*]})" >&2; return 2 ;;
    esac
}

github="${GITHUB_ACTIONS:-}"
failed=()
rows=()
for name in "${selected[@]}"; do
    [ -z "$github" ] || echo "::group::$name"
    started="$(date +%s)"
    run_case "$name"
    status=$?
    seconds=$(( $(date +%s) - started ))
    [ -z "$github" ] || echo "::endgroup::"
    if [ "$status" -eq 0 ]; then
        rows+=("| $name | passed | ${seconds}s |")
    else
        failed+=("$name")
        rows+=("| $name | **failed ($status)** | ${seconds}s |")
        if [ -n "$github" ]; then
            if [ "$blocking" = 1 ]; then level=error; else level=warning; fi
            echo "::$level title=Linux integration::$name failed with status $status (logs: $out/$name)"
        fi
    fi
done

echo
echo "Integration: ${#selected[@]} case(s), ${#failed[@]} failed${failed:+ (${failed[*]})}"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "### Linux integration"
        echo
        echo "| case | result | time |"
        echo "| --- | --- | --- |"
        printf '%s\n' "${rows[@]}"
        if [ "${#failed[@]}" -gt 0 ] && [ "$blocking" = 0 ]; then
            echo
            echo "Reported, not failing: set the repository variable \`MAICORD_LINUX_INTEGRATION_BLOCKING\` to \`true\` to gate on them."
        fi
    } >> "$GITHUB_STEP_SUMMARY"
fi

[ "${#failed[@]}" -eq 0 ] || [ "$blocking" = 0 ]
