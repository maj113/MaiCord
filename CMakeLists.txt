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
    project(MaiCord VERSION 0.6.6 LANGUAGES C CXX)
else()
    project(MaiCord VERSION 0.6.6 LANGUAGES C CXX RC ASM_MASM)
endif()

# Shipping build: MaiCord.exe. Targets defined up to
# cmake/tests.cmake are production targets. Their intermediate products are
# removed by runtime-only finalization after the wrapper finishes.
include(cmake/build_contract.cmake)     # compiler/linker contract, maicord_library/maicord_program
if(MAICORD_LINUX)
    # What the Linux port builds: the libraries (cmake/linux_libraries.cmake), the client (cmake/linux_client.cmake)
    # and its tests (cmake/linux_tests.cmake).
    include(cmake/linux_libraries.cmake)
    include(cmake/linux_client.cmake)
    include(cmake/linux_tests.cmake)
    return()
endif()
include(cmake/generated_sources.cmake)  # packed UI data, shaders, icon, sounds
include(cmake/libraries.cmake)          # shipping static libraries
include(cmake/client.cmake)             # MaiCord.exe, updater identity, cold-code /Os
include(cmake/reference_tools.cmake)    # opt-in: MAICORD_BUILD_FONT_REFERENCE

# Temporary and opt-in targets.
include(cmake/tests.cmake)              # MAICORD_BUILD_TESTS (tools/build.ps1 -Test)
include(cmake/perf_probe.cmake)         # MAICORD_BUILD_PERF_PROBE (tools/build.ps1 -PerfProbe)
include(cmake/benchmarks.cmake)         # MAICORD_BUILD_BENCHMARKS (tools/benchmark_*.ps1)
include(cmake/integration.cmake)        # MAICORD_BUILD_INTEGRATION_TESTS (tools/probe_*.ps1)
include(cmake/build_outputs.cmake)      # transient ownership for runtime-only cleanup
