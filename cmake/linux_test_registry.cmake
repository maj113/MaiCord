# The tests the Linux build registers: the Linux-buildable subset of cmake/test_registry.cmake, under the same test
# names (maicord_core, maicord_byte_order), plus the Linux runtime's own. Sources are the shared tests/ files; a
# platform difference inside one is an #if in the test. Not ported: tests/core/process_accessors.cpp,
# client_startup_imports.cpp and delay_load_helper.cpp, which test Windows interfaces (the TEB accessors, the PE
# import table and the delay-load helper).
maicord_test_bundle(MaiCordCoreUnitTests LIBS maicord_core)

# The core tests of tests/core_test_cases.inc that need nothing outside core, run in one process in that order
# (tests/support/test_main_linux.cpp).
maicord_test(MaiCordTests ENTRY maicord_tests_entry
    SOURCES tests/support/test_main_linux.cpp tests/core/core_runtime.cpp tests/core/json_scan.cpp
        tests/core/clock.cpp tests/core/json_reader_visit.cpp tests/core/json_string_decode.cpp
        tests/core/mailbox_release_race.cpp tests/core/mailbox_acquire_release.cpp
    LIBS maicord_core
    TEST maicord_core)

maicord_test(MaiCordByteOrderTests ENTRY maicord_byte_order_entry SOURCES tests/core/byte_order_test.cpp
    BUNDLE MaiCordCoreUnitTests TEST maicord_byte_order)

# The runtime: process entry state, the pinned libc imports, the library search and the XDG data path.
maicord_test(MaiCordLinuxRuntimeTests ENTRY maicord_linux_runtime_entry SOURCES tests/core/linux_runtime.cpp
    BUNDLE MaiCordCoreUnitTests TIMEOUT 5 TEST maicord_linux_runtime)
maicord_add_tests(MaiCordLinuxRuntimeTests TIMEOUT 5
    TEST maicord_linux_data_path_create --data-path-create)
maicord_add_tests(MaiCordLinuxRuntimeTests TIMEOUT 5
    ENV MAICORD_LIBRARY_PATH=/maicord-no-such-directory:/maicord-another
    TEST maicord_linux_library_path --library-path)
maicord_add_tests(MaiCordLinuxRuntimeTests TIMEOUT 5
    ENV XDG_DATA_HOME=/maicord-xdg-test/data HOME=/maicord-home-test
    TEST maicord_linux_data_path_xdg --data-path-xdg)
maicord_add_tests(MaiCordLinuxRuntimeTests TIMEOUT 5
    ENV --unset=XDG_DATA_HOME HOME=/maicord-home-test
    TEST maicord_linux_data_path_home --data-path-home)
maicord_add_tests(MaiCordLinuxRuntimeTests TIMEOUT 5
    ENV XDG_DATA_HOME=relative/data HOME=/maicord-home-test
    TEST maicord_linux_data_path_relative_xdg --data-path-relative-xdg)
