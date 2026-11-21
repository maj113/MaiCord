cmake_minimum_required(VERSION 3.28)

# The Windows build: one Release configuration. The CMake tree (the cache, CMakeFiles, build.ninja) is
# MaiCord-build/Release/cmake, and the images and runtime assets go to MaiCord-build/Release (MAICORD_OUTPUT_DIRECTORY).
# The name has no leading dot: the plugin loader refuses such a path component, and test profiles live in the tree.
# The binary directory is checked before compiler discovery writes anything. The Linux build (tools/build_linux.sh) is
# configured by the script into its own directory, which also holds the images.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(MAICORD_LINUX ON)
    set(MAICORD_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
else()
    file(REAL_PATH "${CMAKE_SOURCE_DIR}/../MaiCord-build/Release" MAICORD_OUTPUT_DIRECTORY)
    file(REAL_PATH "${CMAKE_BINARY_DIR}" maicord_binary_directory)
    string(TOLOWER "${MAICORD_OUTPUT_DIRECTORY}/cmake" maicord_expected_directory)
    string(TOLOWER "${maicord_binary_directory}" maicord_binary_directory)
    if(NOT maicord_expected_directory STREQUAL maicord_binary_directory)
        message(FATAL_ERROR "Use the single MaiCord-build/Release/cmake build directory (CMakePresets.json).")
    endif()
endif()
if(CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "MaiCord uses one Release configuration.")
endif()
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)

if(MAICORD_LINUX)
    project(MaiCord VERSION 0.8.2 LANGUAGES C CXX)
else()
    # MASM (src/core/runtime_x64.asm) is the Build Tools' ml64 (tools/build.ps1 names it): llvm-ml encodes
    # register-to-register instructions the other valid way, which would change MaiCord.exe.
    project(MaiCord VERSION 0.8.2 LANGUAGES C CXX RC ASM_MASM)
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
include(cmake/generated_sources.cmake)  # packed UI data, shaders, sounds, Wayland bindings
include(cmake/libraries.cmake)          # shipping static libraries
include(cmake/client.cmake)             # MaiCord, updater identity, image audit, cold-code size policy

# Opt-in targets.
include(cmake/tests.cmake)              # MAICORD_BUILD_TESTS (tools/build.ps1 -Test, tools/build_linux.sh --test)
if(MAICORD_LINUX)
    return()
endif()
include(cmake/perf_probe.cmake)         # MAICORD_BUILD_PERF_PROBE (tools/build.ps1 -PerfProbe)
include(cmake/build_outputs.cmake)      # transient ownership for runtime-only cleanup
