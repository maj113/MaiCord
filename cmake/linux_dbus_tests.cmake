# The tests of the D-Bus client (cmake/linux_dbus.cmake), registered from linux_test_registry.cmake.
#
#   maicord_dbus_wire       marshalling, header parsing, hostile input and addresses; needs no bus.
#   maicord_atspi_map       the AT-SPI server's objects (roles, states, interfaces, actions, text, the answers to calls on a
#                           synthetic window); needs no bus.
#
# The integration tests talk to private buses that tools/test_dbus.sh starts (dbus-daemon and the python mock services);
# without MAICORD_DBUS_TEST_BUS in the environment they skip with status 77, so they sit in the normal suite and never
# touch the bus of the user's session. (cmake/linux_tests.cmake gives every test MAICORD_SECRET_SERVICE=off, so that no
# test reaches the user's keyring; the secure buffer tests below ask for `auto`, against the mock service.)
#
#   maicord_dbus_bus              the connection: authentication, calls, timeouts, typed bodies decoded by libdbus,
#                                 descriptors, signals, an exported object, a fake bus that sends garbage
#   maicord_dbus_services         the settings portal, notifications through the daemon, OpenURI, the Secret Service and
#                                 the portal probes, against the mock services
#   maicord_dbus_services_portal  notifications through the portal (the script sets MAICORD_NOTIFICATION_BACKEND=portal)
#   maicord_dbus_services_plain   notifications through a daemon that does not read markup in the body
#   maicord_dbus_services_bare    every client on a bus without any service
#   maicord_dbus_desktop          the tray (StatusNotifierItem and dbusmenu), GlobalShortcuts, the file chooser, the launcher
#                                 badge and single instance on the bus, against the mock; _bare: all of it without services
#   maicord_dbus_instance_socket  single instance over the abstract socket (no bus needed: always runs)
#   maicord_dbus_secure_buffer    the user-bound secure buffer with its master key in the Secret Service (creation,
#                                 lookup, the fallback to the key file, moving the file in), and on a bus without one
#   maicord_dbus_real             the settings portal, the probes and the Secret Service against xdg-desktop-portal and
#                                 gnome-keyring on a private bus (tools/test_dbus_real.sh; skips without MAICORD_DBUS_REAL=1)
#   maicord_desktop_entry         the generated .desktop file (tools/generate_desktop_entry.py --validate), and
#                                 desktop-file-validate when it is installed
maicord_test(MaiCordDbusWireTests ENTRY maicord_dbus_wire_entry
    SOURCES tests/platform/dbus/dbus_wire.cpp
    LIBS maicord_dbus maicord_core
    DEPENDS maicord_dbus
    TIMEOUT 20 TEST maicord_dbus_wire)

# The AT-SPI server's objects (src/platform/atspi) without a bus: the role, state, interface and action mapping, the paths,
# the text offsets, and the answers to the calls a screen reader makes on a synthetic window. tools/test_atspi.sh drives the
# real client through the real registry and a client library.
maicord_test(MaiCordAtspiMapTests ENTRY maicord_atspi_map_entry
    SOURCES tests/platform/atspi/atspi_map.cpp
    LIBS maicord_atspi maicord_ui maicord_dbus maicord_core
    DEPENDS maicord_atspi
    TIMEOUT 20 TEST maicord_atspi_map)

# The server's life on a bare private bus (tools/test_dbus.sh), with this program as org.a11y.Bus, the registry and the
# screen reader: nothing connects while accessibility is off, IsEnabled or ScreenReaderEnabled brings the connection and the
# registration, a client's call, the events of a snapshot difference, off again, and a registry that returns.
maicord_test(MaiCordAtspiBusTests ENTRY maicord_atspi_bus_entry
    SOURCES tests/platform/atspi/atspi_bus.cpp
    LIBS maicord_atspi maicord_ui maicord_dbus maicord_core
    DEPENDS maicord_atspi
    TIMEOUT 30 TEST maicord_atspi_bus)

maicord_test(MaiCordDbusBusTests ENTRY maicord_dbus_bus_entry
    SOURCES tests/platform/dbus/dbus_bus.cpp
    LIBS maicord_dbus maicord_core
    DEPENDS maicord_dbus
    TIMEOUT 20 TEST maicord_dbus_bus)

maicord_test(MaiCordDbusServicesTests ENTRY maicord_dbus_services_entry
    SOURCES tests/platform/dbus/dbus_services.cpp
    LIBS maicord_dbus maicord_core
    DEPENDS maicord_dbus
    TIMEOUT 20 TEST maicord_dbus_services)
maicord_add_tests(MaiCordDbusServicesTests TIMEOUT 20 TEST maicord_dbus_services_portal --portal)
maicord_add_tests(MaiCordDbusServicesTests TIMEOUT 20 TEST maicord_dbus_services_plain --plain)
maicord_add_tests(MaiCordDbusServicesTests TIMEOUT 20 TEST maicord_dbus_services_bare --bare)

maicord_test(MaiCordDbusDesktopTests ENTRY maicord_dbus_desktop_entry
    SOURCES tests/platform/dbus/dbus_desktop.cpp
    LIBS maicord_dbus maicord_core
    DEPENDS maicord_dbus
    TIMEOUT 20 TEST maicord_dbus_desktop)
maicord_add_tests(MaiCordDbusDesktopTests TIMEOUT 20 TEST maicord_dbus_desktop_bare --bare)
maicord_add_tests(MaiCordDbusDesktopTests TIMEOUT 20 ENV MAICORD_INSTANCE_BACKEND=socket
    TEST maicord_dbus_instance_socket --socket)

maicord_test(MaiCordSecureBufferServiceTests ENTRY maicord_secure_buffer_service_entry
    SOURCES tests/platform/dbus/secure_buffer_service.cpp
    LIBS maicord_crypto maicord_dbus maicord_core
    DEPENDS maicord_dbus
    TIMEOUT 20 TEST maicord_dbus_secure_buffer)
maicord_add_tests(MaiCordSecureBufferServiceTests TIMEOUT 20 TEST maicord_dbus_secure_buffer_bare --bare)

maicord_test(MaiCordDbusRealTests ENTRY maicord_dbus_real_entry
    SOURCES tests/platform/dbus/dbus_real.cpp
    LIBS maicord_crypto maicord_dbus maicord_core
    DEPENDS maicord_dbus
    TIMEOUT 20 TEST maicord_dbus_real)

foreach(maicord_dbus_case IN ITEMS maicord_dbus_bus maicord_dbus_services maicord_dbus_services_portal
        maicord_dbus_services_plain maicord_dbus_services_bare maicord_dbus_desktop maicord_dbus_desktop_bare maicord_dbus_secure_buffer
        maicord_dbus_secure_buffer_bare maicord_dbus_real maicord_atspi_bus)
    # Status 77: no private bus to talk to. The mock services serve one test at a time.
    set_tests_properties(${maicord_dbus_case} PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK dbus_private_bus)
endforeach()
foreach(maicord_dbus_case IN ITEMS maicord_dbus_secure_buffer maicord_dbus_secure_buffer_bare)
    set_property(TEST ${maicord_dbus_case} APPEND PROPERTY ENVIRONMENT "MAICORD_SECRET_SERVICE=auto")
endforeach()
set_tests_properties(maicord_dbus_instance_socket PROPERTIES SKIP_RETURN_CODE 77)

# The packaging artifact: the generated .desktop file is valid (our checks, then desktop-file-validate where installed).
add_test(NAME maicord_desktop_entry
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_desktop_entry.py"
        --validate "${MAICORD_DESKTOP_ENTRY}")
add_test(NAME maicord_desktop_entry_validate
    COMMAND sh -c "command -v desktop-file-validate >/dev/null || exit 77; desktop-file-validate \"$1\"" validate
        "${MAICORD_DESKTOP_ENTRY}")
set_tests_properties(maicord_desktop_entry_validate PROPERTIES SKIP_RETURN_CODE 77)
