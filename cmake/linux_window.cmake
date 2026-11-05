# The Wayland window backend of the Linux build (src/platform/window/wayland), behind include/maicord/platform/window.
# It never enters the Windows build: that one compiles src/platform/window/win32.
#
# The protocol bindings are generated at build time by tools/generate_wayland_protocols.py from the XML in
# third_party/wayland (wayland-scanner is not involved); libwayland-client, libwayland-egl, libwayland-cursor and
# libxkbcommon are dlopen()ed, so the image gains no DT_NEEDED.
set(MAICORD_WAYLAND_XML_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wayland")
# File order is output order; every interface another names must be listed (cursor-shape names the tablet tool).
set(MAICORD_WAYLAND_XML
    "${MAICORD_WAYLAND_XML_DIRECTORY}/wayland.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/xdg-shell.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/viewporter.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/presentation-time.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/tablet-v2.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/fractional-scale-v1.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/cursor-shape-v1.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/xdg-activation-v1.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/ext-idle-notify-v1.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/xdg-toplevel-icon-v1.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/text-input-unstable-v3.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/primary-selection-unstable-v1.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/xdg-decoration-unstable-v1.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/idle-inhibit-unstable-v1.xml"
    # The compositor side of input injection, for tests/platform/window/wayland_window.cpp (wlroots compositors).
    "${MAICORD_WAYLAND_XML_DIRECTORY}/wlr-virtual-pointer-unstable-v1.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/virtual-keyboard-unstable-v1.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/input-method-unstable-v2.xml"
    "${MAICORD_WAYLAND_XML_DIRECTORY}/wlr-data-control-unstable-v1.xml")
set(MAICORD_WAYLAND_GENERATED_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/wayland")
set(MAICORD_WAYLAND_PROTOCOL_HEADER "${MAICORD_WAYLAND_GENERATED_DIRECTORY}/wayland_protocols.hpp")
set(MAICORD_WAYLAND_PROTOCOL_SOURCE "${MAICORD_WAYLAND_GENERATED_DIRECTORY}/wayland_protocols.cpp")
find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
add_custom_command(
    OUTPUT "${MAICORD_WAYLAND_PROTOCOL_HEADER}" "${MAICORD_WAYLAND_PROTOCOL_SOURCE}"
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_wayland_protocols.py"
        "${MAICORD_WAYLAND_GENERATED_DIRECTORY}" ${MAICORD_WAYLAND_XML}
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_wayland_protocols.py" ${MAICORD_WAYLAND_XML}
    VERBATIM)

# maicord_dbus: the desktop settings (the settings portal) and the session bus the loop waits on.
maicord_library(maicord_window_wayland
    src/platform/window/wayland/client.cpp src/platform/window/wayland/xkb.cpp
    src/platform/window/wayland/keymap.cpp src/platform/window/wayland/window.cpp
    src/platform/window/wayland/pointer.cpp src/platform/window/wayland/keyboard.cpp
    src/platform/window/wayland/pump.cpp src/platform/window/wayland/global_keys.cpp
    src/platform/window/wayland/frame.cpp src/platform/window/wayland/extras.cpp
    src/platform/window/wayland/data.cpp src/platform/window/wayland/uri.cpp
    src/platform/window/wayland/text_input.cpp
    # Copy Image encodes its PNG with the clipboard's own encoder, which the Windows backend shares.
    src/platform/desktop/clipboard_image.cpp
    "${MAICORD_WAYLAND_PROTOCOL_HEADER}" "${MAICORD_WAYLAND_PROTOCOL_SOURCE}"
    LIBS maicord_core maicord_media_codec maicord_dbus INCLUDES "${MAICORD_WAYLAND_GENERATED_DIRECTORY}")
