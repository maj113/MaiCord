# The D-Bus client of the Linux build (src/platform/dbus, include/maicord/platform/dbus): the wire format, the bus
# connection over the Unix socket, and the desktop services over it (settings portal, notifications, OpenURI, the Secret
# Service, portal probes, the tray, global shortcuts, single instance, the file chooser, the launcher badge). It never
# enters the Windows build; libc's posix_spawnp (xdg-open fallback) is looked up at run time, so the image gains no
# DT_NEEDED and no import.
maicord_library(maicord_dbus
    src/platform/dbus/wire.cpp src/platform/dbus/connection.cpp src/platform/dbus/bus.cpp
    src/platform/dbus/session.cpp
    src/platform/dbus/desktop_settings.cpp src/platform/dbus/notifications.cpp src/platform/dbus/open_uri.cpp
    src/platform/dbus/secret_service.cpp src/platform/dbus/portal.cpp
    src/platform/dbus/tray.cpp src/platform/dbus/global_shortcuts.cpp src/platform/dbus/instance.cpp
    src/platform/dbus/file_chooser.cpp src/platform/dbus/launcher_entry.cpp
    LIBS maicord_core)

# The AT-SPI server over that client (src/platform/atspi, include/maicord/platform/atspi): the application's objects on the
# accessibility bus for screen readers, answered from the accessibility snapshot (ui/accessibility). Linux only, like
# the UI Automation server it stands beside (maicord_uia, Windows only). Nothing connects until org.a11y.Status says an
# assistive technology is on.
maicord_library(maicord_atspi
    src/platform/atspi/map.cpp src/platform/atspi/accessible.cpp src/platform/atspi/server.cpp
    src/platform/atspi/events.cpp
    LIBS maicord_dbus maicord_ui maicord_core)
