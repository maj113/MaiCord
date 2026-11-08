cmake_minimum_required(VERSION 3.28)

# The Windows build: one Release configuration in the sibling MaiCord-build/Release directory,
# checked before compiler discovery writes anything. The Linux build (tools/build_linux.sh) is
# configured by the script into its own directory.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(MAICORD_LINUX ON)
else()
    file(REAL_PATH "${CMAKE_SOURCE_DIR}/../MaiCord-build/Release" maicord_release_directory)
    file(REAL_PATH "${CMAKE_BINARY_DIR}" maicord_binary_directory)
    string(TOLOWER "${maicord_release_directory}" maicord_release_directory)
    string(TOLOWER "${maicord_binary_directory}" maicord_binary_directory)
    if(NOT maicord_release_directory STREQUAL maicord_binary_directory)
        message(FATAL_ERROR "Use the single MaiCord-build/Release build directory.")
    endif()
endif()
if(CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "MaiCord uses one Release configuration.")
endif()
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)

if(MAICORD_LINUX)
    project(MaiCord VERSION 0.7.0 LANGUAGES C CXX)
else()
    # MASM (src/core/runtime_x64.asm) is the Build Tools' ml64 (tools/build.ps1 names it): llvm-ml encodes
    # register-to-register instructions the other valid way, which would change MaiCord.exe.
    project(MaiCord VERSION 0.7.0 LANGUAGES C CXX RC ASM_MASM)
    if(NOT CMAKE_ASM_MASM_COMPILER MATCHES "[/\\\\]ml64\\.exe$")
        message(FATAL_ERROR "MaiCord assembles with the MSVC Build Tools' ml64.exe (CMAKE_ASM_MASM_COMPILER=${CMAKE_ASM_MASM_COMPILER}).")
    endif()
endif()

# The test-only environment switches (include/maicord/core/test_hook.hpp). On Linux a build with tests compiles them
# into everything, its client included, which is then not a shipping build. On Windows the tests don't need them, and
# tools/build.ps1 -TestHooks builds a client with them alone, which can be pointed at tools/fake_discord_server.py. The
# audit that guards shipping images rejects either client.
option(MAICORD_BUILD_TESTS "Enable temporary deterministic test targets" OFF)
if(MAICORD_LINUX)
    set(maicord_test_hooks ${MAICORD_BUILD_TESTS})
else()
    option(MAICORD_CLIENT_TEST_HOOKS "Compile the test-only environment switches into the client" OFF)
    set(maicord_test_hooks ${MAICORD_CLIENT_TEST_HOOKS})
endif()
if(maicord_test_hooks)
    add_compile_definitions(MAICORD_TEST_HOOKS=1)
endif()

# Shipping build: the client. Targets defined up to cmake/tests.cmake are
# production targets. Their intermediate products are removed by runtime-only
# finalization after the build script finishes.
include(cmake/build_contract.cmake)     # compiler/linker contract, maicord_library/maicord_program
include(cmake/generated_sources.cmake)  # packed UI data, shaders, icon, sounds, Wayland bindings
include(cmake/libraries.cmake)          # shipping static libraries
include(cmake/client.cmake)             # MaiCord, updater identity, image audit, cold-code size policy

# Temporary and opt-in targets.
if(NOT MAICORD_LINUX)
    include(cmake/reference_tools.cmake)    # opt-in: MAICORD_BUILD_FONT_REFERENCE
endif()
include(cmake/tests.cmake)              # MAICORD_BUILD_TESTS (tools/build.ps1 -Test, tools/build_linux.sh --test)
if(MAICORD_LINUX)
    return()
endif()
include(cmake/perf_probe.cmake)         # MAICORD_BUILD_PERF_PROBE (tools/build.ps1 -PerfProbe)
include(cmake/integration.cmake)        # MAICORD_BUILD_INTEGRATION_TESTS (tools/probe_network.ps1, tools/opus_conformance.ps1)
include(cmake/build_outputs.cmake)      # transient ownership for runtime-only cleanup
