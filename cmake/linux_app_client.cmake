# The application proper on Linux: every unit of src/app that the transport, updater and package libraries
# (linux_app_libraries.cmake) do not already hold, as one library (cmake/client.cmake lists them for Windows). The
# shell (src/app/shell) belongs to the window layer and is not here, nor are src/app/windows (the Win32 entry and
# window procedure) and src/app/linux (the Linux entry and window glue): those are the programs' own (the client,
# cmake/linux_client.cmake). accessibility.cpp is the UI Automation server (the units that stand in for it are in
# src/app/accessibility/linux: the AT-SPI server's host, over platform/atspi); the snapshot (accessibility_describe.cpp)
# and the actions it routes (accessibility_actions.cpp) are shared. src/app/update/package.cpp, the zip
# and PE reader of the Windows updater, is not here either: src/app/update/linux/package.cpp (in maicord_app_update)
# reads the Linux tarball instead.
file(GLOB_RECURSE MAICORD_APP_CLIENT_SOURCES CONFIGURE_DEPENDS RELATIVE "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/app/*.cpp")
list(FILTER MAICORD_APP_CLIENT_SOURCES EXCLUDE REGEX
    "^src/app/shell/|^src/app/windows/|^src/app/linux/|^src/app/accessibility/accessibility\\.cpp$|^src/app/update/package\\.cpp$")
foreach(library maicord_app_net maicord_app_update maicord_app_packages maicord_app_catalog_index)
    get_target_property(held_sources ${library} SOURCES)
    list(REMOVE_ITEM MAICORD_APP_CLIENT_SOURCES ${held_sources})
endforeach()

maicord_library(maicord_app_client ${MAICORD_APP_CLIENT_SOURCES}
    # The desktop integration the application drives (platform/desktop): launch and link parsing, the Linux startup and
    # protocol registration, the taskbar badge, the notification-area icon's drawing and plans, the sign-in launcher.
    src/platform/desktop/integration.cpp src/platform/desktop/linux/integration.cpp
    src/platform/desktop/taskbar_attention.cpp src/platform/desktop/system_tray.cpp
    src/platform/desktop/browser_sign_in_launcher.cpp src/platform/desktop/linux/fido2.cpp
    src/platform/desktop/linux/webauthn_ceremony.cpp
    src/platform/desktop/linux/notifications.cpp src/platform/desktop/linux/notification_identity.cpp
    # The file dialog (the FileChooser portal) and the dropped and pasted files it checks.
    src/platform/desktop/linux/file_picker.cpp
    src/model/chat/message_reconcile.cpp
    LIBS maicord_app_net maicord_app_update maicord_app_packages maicord_app_catalog_index maicord_ui maicord_core maicord_model
        maicord_protocol maicord_media_core maicord_media_codec maicord_compression maicord_crypto
        maicord_desktop_system maicord_dbus maicord_atspi
    INCLUDES src/app "${MAICORD_GENERATED_DIRECTORY}")
target_compile_options(maicord_app_client PRIVATE -Wno-c23-extensions)

# The secure text field: the edit model, its history and target, and the Linux text store (the Windows one is the
# message-driven store in secure_text_store.cpp).
maicord_library(maicord_ui_secure_text
    src/ui/text/secure_edit.cpp src/ui/text/secure_edit_history.cpp src/ui/text/secure_edit_target.cpp
    src/ui/text/linux/secure_text_store.cpp
    LIBS maicord_ui maicord_core maicord_crypto INCLUDES src)

# The headless window backend (src/platform/window/null) behind the same service as the Wayland one, for tests that
# drive handlers without a compositor. Tests list it ahead of maicord_window_wayland when both are linked.
maicord_library(maicord_window_null src/platform/window/null/window.cpp LIBS maicord_core)
