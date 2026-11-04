# The tests of the Wayland window backend (cmake/linux_window.cmake), registered from linux_test_registry.cmake.
#
#   maicord_wayland_input     the pure parts: the key table, characters, double click, key repeat with an injected
#                             clock, the generated protocol tables against libwayland-client's and the key table over
#                             real xkb layouts (the last two report SKIP when the libraries are not installed).
#
# The integration tests need a compositor, so they are opt-in: configure with MAICORD_WAYLAND_INTEGRATION=1 in the
# environment (tools/test_wayland_window.sh does, after starting sway) and they run; otherwise they are registered
# disabled. They assert the WindowEvents of a real window.
#
#   maicord_wayland_window_lifecycle   the window opens, is configured and acked, answers frame callbacks, presents a
#                                      wl_shm clear and closes; any compositor (sway, Weston, WSLg)
#   maicord_wayland_window_input       pointer and key events injected into the window (sway: swaymsg and wtype)
maicord_test(MaiCordWaylandInputTests ENTRY maicord_wayland_input_entry
    SOURCES tests/platform/window/wayland_input.cpp
    LIBS maicord_window_wayland maicord_core
    INCLUDES "${MAICORD_WAYLAND_GENERATED_DIRECTORY}"
    DEPENDS maicord_window_wayland
    TIMEOUT 10 TEST maicord_wayland_input)

maicord_test(MaiCordWaylandWindowTests ENTRY maicord_wayland_window_entry
    SOURCES tests/platform/window/wayland_window.cpp
    LIBS maicord_window_wayland maicord_core
    INCLUDES "${MAICORD_WAYLAND_GENERATED_DIRECTORY}"
    DEPENDS maicord_window_wayland
    TIMEOUT 20 LABELS integration TEST maicord_wayland_window_lifecycle --lifecycle)
maicord_add_tests(MaiCordWaylandWindowTests TIMEOUT 20 LABELS integration
    TEST maicord_wayland_window_input --input)
foreach(maicord_wayland_case IN ITEMS maicord_wayland_window_lifecycle maicord_wayland_window_input)
    # Status 77: no compositor to talk to. One window at a time on the compositor.
    set_tests_properties(${maicord_wayland_case} PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK wayland_compositor)
    if(NOT "$ENV{MAICORD_WAYLAND_INTEGRATION}")
        set_tests_properties(${maicord_wayland_case} PROPERTIES DISABLED TRUE)
    endif()
endforeach()
# The script's directory for the files it shares with a test: one subdirectory a case (lifecycle, input).
if(DEFINED ENV{MAICORD_WAYLAND_SYNC_DIR})
    set_tests_properties(maicord_wayland_window_lifecycle PROPERTIES
        ENVIRONMENT "MAICORD_WAYLAND_SYNC_DIR=$ENV{MAICORD_WAYLAND_SYNC_DIR}/lifecycle")
    set_tests_properties(maicord_wayland_window_input PROPERTIES
        ENVIRONMENT "MAICORD_WAYLAND_SYNC_DIR=$ENV{MAICORD_WAYLAND_SYNC_DIR}/input")
endif()
