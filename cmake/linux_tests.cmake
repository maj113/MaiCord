# Temporary deterministic tests of the Linux build (tools/build_linux.sh --test; cmake/tests.cmake is the Windows
# configuration). Only the tests of the code the Linux build has ported are registered, in
# cmake/linux_test_registry.cmake; cmake/test_support.cmake and the shared tests/ sources serve both platforms.
option(MAICORD_BUILD_TESTS "Enable temporary deterministic test targets" OFF)
if(NOT MAICORD_BUILD_TESTS)
    return()
endif()

include(cmake/test_support.cmake)
enable_testing()
include(cmake/linux_test_registry.cmake)
maicord_finish_test_bundles()

# Every test finishes within 20 s and runs with a private data directory (the tests' XDG_DATA_HOME, as
# tools/build.ps1 gives each Windows test a private LOCALAPPDATA).
get_property(maicord_registered_tests DIRECTORY PROPERTY TESTS)
foreach(maicord_registered_test IN LISTS maicord_registered_tests)
    get_test_property(${maicord_registered_test} TIMEOUT maicord_test_timeout)
    if(NOT maicord_test_timeout OR maicord_test_timeout GREATER 20)
        set_property(TEST ${maicord_registered_test} PROPERTY TIMEOUT 20)
    endif()
    get_test_property(${maicord_registered_test} ENVIRONMENT maicord_test_environment)
    if(NOT maicord_test_environment MATCHES "(^|;)XDG_DATA_HOME=")
        set_property(TEST ${maicord_registered_test} APPEND PROPERTY
            ENVIRONMENT "XDG_DATA_HOME=${CMAKE_BINARY_DIR}/test-profile/${maicord_registered_test}")
    endif()
endforeach()
