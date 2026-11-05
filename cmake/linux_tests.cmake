# Temporary deterministic tests of the Linux build (tools/build_linux.sh --test; cmake/tests.cmake is the Windows
# configuration). Only the tests of the code the Linux build has ported are registered, in
# cmake/linux_test_registry.cmake; cmake/test_support.cmake and the shared tests/ sources serve both platforms.
if(NOT MAICORD_BUILD_TESTS)
    return()
endif()

include(cmake/test_support.cmake)
enable_testing()
include(cmake/linux_test_registry.cmake)
maicord_finish_test_bundles()

# Every test finishes within 20 s (slower hosted runners scale each limit with MAICORD_TEST_TIMEOUT_SCALE, which
# tools/build_linux.sh --timeout-scale sets) and runs with private data, state and cache directories (the tests'
# XDG_DATA_HOME, XDG_STATE_HOME and XDG_CACHE_HOME, as tools/build.ps1 gives each Windows test a private LOCALAPPDATA).
set(MAICORD_TEST_TIMEOUT_SCALE 1 CACHE STRING "Multiplier applied to every per-test timeout (hosted CI runners)")
get_property(maicord_registered_tests DIRECTORY PROPERTY TESTS)
foreach(maicord_registered_test IN LISTS maicord_registered_tests)
    get_test_property(${maicord_registered_test} TIMEOUT maicord_test_timeout)
    if(NOT maicord_test_timeout OR maicord_test_timeout GREATER 20)
        set_property(TEST ${maicord_registered_test} PROPERTY TIMEOUT 20)
        set(maicord_test_timeout 20)
    endif()
    if(MAICORD_TEST_TIMEOUT_SCALE GREATER 1 AND maicord_test_timeout MATCHES "^[0-9]+$")
        math(EXPR maicord_test_timeout "${maicord_test_timeout} * ${MAICORD_TEST_TIMEOUT_SCALE}")
        set_property(TEST ${maicord_registered_test} PROPERTY TIMEOUT ${maicord_test_timeout})
    endif()
    get_test_property(${maicord_registered_test} ENVIRONMENT maicord_test_environment)
    if(NOT maicord_test_environment MATCHES "(^|;)XDG_DATA_HOME=")
        set_property(TEST ${maicord_registered_test} APPEND PROPERTY
            ENVIRONMENT "XDG_DATA_HOME=${CMAKE_BINARY_DIR}/test-profile/${maicord_registered_test}")
    endif()
    # The log lives under the state directory (D19), and nothing a test writes may reach the user's own.
    if(NOT maicord_test_environment MATCHES "(^|;)XDG_STATE_HOME=")
        set_property(TEST ${maicord_registered_test} APPEND PROPERTY
            ENVIRONMENT "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-profile/${maicord_registered_test}/state")
    endif()
    if(NOT maicord_test_environment MATCHES "(^|;)XDG_CACHE_HOME=")
        set_property(TEST ${maicord_registered_test} APPEND PROPERTY
            ENVIRONMENT "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/test-profile/${maicord_registered_test}/cache")
    endif()
    # Nothing a test does may reach the user's keyring: the master key stays in the test profile's key file, unless a test
    # asks for the Secret Service (MAICORD_SECRET_SERVICE=auto, against the mock service of tools/test_dbus.sh).
    if(NOT maicord_test_environment MATCHES "(^|;)MAICORD_SECRET_SERVICE=")
        set_property(TEST ${maicord_registered_test} APPEND PROPERTY ENVIRONMENT "MAICORD_SECRET_SERVICE=off")
    endif()
endforeach()
