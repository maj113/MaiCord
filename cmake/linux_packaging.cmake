# The desktop integration files a Linux package installs (tools/generate_desktop_entry.py): the .desktop file named after
# the application id, with MimeType=x-scheme-handler/discord and maicord, and the hicolor icon PNGs of tools/generate_app_icon.py.
# They land in <build>/packaging/{applications,icons}, the layout of an install prefix's share directory.
find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
set(MAICORD_PACKAGING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packaging")
set(MAICORD_DESKTOP_ENTRY "${MAICORD_PACKAGING_DIRECTORY}/applications/io.github.maj113.MaiCord.desktop")
add_custom_command(
    OUTPUT "${MAICORD_DESKTOP_ENTRY}" "${MAICORD_PACKAGING_DIRECTORY}/icons/hicolor/256x256/apps/io.github.maj113.MaiCord.png"
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_desktop_entry.py"
        --output "${MAICORD_PACKAGING_DIRECTORY}" --icons
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_desktop_entry.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_app_icon.py"
    VERBATIM)
add_custom_target(maicord_desktop_entry ALL DEPENDS "${MAICORD_DESKTOP_ENTRY}")
