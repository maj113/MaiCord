cmake_minimum_required(VERSION 3.28)

# Reject source-contained caches before compiler discovery creates output.
# Resolve links and compare case-insensitively on the Windows build host.
function(maicord_require_external_path candidate description)
    file(REAL_PATH "${CMAKE_SOURCE_DIR}" source_root)
    file(REAL_PATH "${candidate}" output_root BASE_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_HOST_WIN32)
        string(TOLOWER "${source_root}" source_root)
        string(TOLOWER "${output_root}" output_root)
    endif()
    cmake_path(IS_PREFIX source_root "${output_root}" NORMALIZE inside_source)
    if(inside_source)
        message(FATAL_ERROR "${description} must be outside the MaiCord source tree. Use the sibling MaiCord-build directory.")
    endif()
endfunction()
maicord_require_external_path("${CMAKE_BINARY_DIR}" "Build directory")

file(REAL_PATH "${CMAKE_SOURCE_DIR}/../MaiCord-build/Release" maicord_release_directory)
file(REAL_PATH "${CMAKE_BINARY_DIR}" maicord_binary_directory)
string(TOLOWER "${maicord_release_directory}" maicord_release_directory_compare)
string(TOLOWER "${maicord_binary_directory}" maicord_binary_directory_compare)
if(NOT maicord_release_directory_compare STREQUAL maicord_binary_directory_compare)
    message(FATAL_ERROR "Use the single MaiCord-build/Release build directory.")
endif()
if(CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "MaiCord uses one Release configuration.")
endif()
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "Build configurations" FORCE)

project(MaiCord VERSION 0.3.3 LANGUAGES C CXX RC ASM_MASM)

# Shipping build: MaiCord.exe and MaiCordAccount.dll. Targets defined up to
# cmake/tests.cmake are production targets. Their intermediate products are
# removed by runtime-only finalization after the wrapper finishes.
include(cmake/build_contract.cmake)     # compiler/linker contract, maicord_library/maicord_program
include(cmake/generated_sources.cmake)  # packed UI data, shaders, icon, sounds
include(cmake/libraries.cmake)          # shipping static libraries
include(cmake/client.cmake)             # MaiCord.exe, MaiCordAccount.dll, updater identity, cold-code /Os
include(cmake/reference_tools.cmake)    # opt-in: MAICORD_BUILD_FONT_REFERENCE

# Temporary and opt-in targets.
include(cmake/tests.cmake)              # MAICORD_BUILD_TESTS (tools/build.ps1 -Test)
include(cmake/perf_probe.cmake)         # MAICORD_BUILD_PERF_PROBE (tools/build.ps1 -PerfProbe)
include(cmake/benchmarks.cmake)         # MAICORD_BUILD_BENCHMARKS (tools/benchmark_*.ps1)
include(cmake/integration.cmake)        # MAICORD_BUILD_INTEGRATION_TESTS (tools/probe_*.ps1)
include(cmake/build_outputs.cmake)      # transient ownership for runtime-only cleanup
