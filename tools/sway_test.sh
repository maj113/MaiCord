#!/usr/bin/env bash
# A function for the scripts that run a test or the client on their own sway (tools/test_wayland_window.sh,
# test_gpu_surface.sh, test_client_wayland.sh, test_atspi.sh, test_dbus_tray.sh): source this file, then call
#
#   sway_test_start CONFIG LOG [COMPOSITOR]   starts sway with CONFIG, logging to LOG, headless (the default) or nested
#                                             as a window of the running compositor (nested), and waits until it serves;
#                                             sets sway_pid and exports WAYLAND_DISPLAY and SWAYSOCK. When sway does not
#                                             come up it stops it and fails, naming the log.
#
# XDG_RUNTIME_DIR must be set: sway puts its sockets there. The renderer is pixman, so no GPU is needed.

sway_test_start() {
    local config="$1" log="$2" compositor="${3:-headless}" display="" socket _
    if [ "$compositor" = headless ]; then
        export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1
    else
        export WLR_BACKENDS=wayland
    fi
    export WLR_RENDERER=pixman
    unset DISPLAY
    # -d: sway names its Wayland display in the log (other compositors may be starting in the same runtime directory, and a
    # stale socket of an earlier run may have the next name, so the directory cannot tell); its IPC socket is named after
    # its pid.
    sway -d -c "$config" > "$log" 2>&1 &
    sway_pid=$!
    socket="$XDG_RUNTIME_DIR/sway-ipc.$(id -u).$sway_pid.sock"
    for _ in $(seq 1 100); do
        display="$(sed -n "s/.*Running compositor on wayland display '\\(.*\\)'.*/\\1/p" "$log" | tail -n 1)"
        [ -n "$display" ] && [ -S "$socket" ] && break
        display=""
        sleep 0.1
    done
    [ -n "$display" ] || { kill "$sway_pid" 2>/dev/null; echo "sway did not start; see $log" >&2; return 1; }
    sleep 0.5
    export WAYLAND_DISPLAY="$display" SWAYSOCK="$socket"
}
