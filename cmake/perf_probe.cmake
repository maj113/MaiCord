# Opt-in performance probe (tools/perf): the shipping client sources built
# with MAICORD_PERF_PROBE and shipping code generation, plus a synthetic
# account module serving a large offline session. tools/build.ps1 -PerfProbe
# configures it, runs tools/perf/run_perf.ps1 and removes it again like the
# temporary test targets (same manifest), so it never coexists with them.
option(MAICORD_BUILD_PERF_PROBE "Build the temporary offline performance probe" OFF)
if(NOT MAICORD_BUILD_PERF_PROBE)
    return()
endif()
if(MAICORD_BUILD_TESTS)
    message(FATAL_ERROR "MAICORD_BUILD_PERF_PROBE and MAICORD_BUILD_TESTS are separate configurations")
endif()
maicord_program(MaiCordPerfAccount SHARED ENTRY maicord_perf_account_entry
    SOURCES tools/perf/perf_account.cpp
    LIBS maicord_core maicord_platform maicord_protocol maicord_app_core maicord_model kernel32
    LINK_OPTIONS /EXPORT:maicord_account_get_api,@133,NONAME /EXPORT:maicord_perf_account_control
        /EXPORT:maicord_perf_account_counters
    PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
get_target_property(maicord_perf_probe_sources MaiCord SOURCES)
# MaiCord's precompiled-header stubs generate MaiCord's own .pch (a second
# target listing them is a duplicate Ninja rule). The probe leaves
# MAICORD_APPLICATION_PCH unset, so its copies of the sources compile without it.
list(FILTER maicord_perf_probe_sources EXCLUDE REGEX "/application_pch_[A-Za-z0-9]+\\.cpp$")
get_target_property(maicord_perf_probe_links MaiCord LINK_LIBRARIES)
add_executable(MaiCordPerfProbe WIN32 ${maicord_perf_probe_sources}
    tools/perf/perf_probe.cpp tools/perf/perf_startup.cpp src/app/account_host.cpp)
target_link_libraries(MaiCordPerfProbe PRIVATE ${maicord_perf_probe_links})
target_include_directories(MaiCordPerfProbe PRIVATE "${MAICORD_GENERATED_DIRECTORY}" src/app)
target_compile_definitions(MaiCordPerfProbe PRIVATE MAICORD_PERF_PROBE=1)
# run_perf.ps1 resolves allocation call stacks through the map.
target_link_options(MaiCordPerfProbe PRIVATE /ENTRY:maicord_entry /SUBSYSTEM:WINDOWS,6.02
    "/MAP:${CMAKE_CURRENT_BINARY_DIR}/MaiCordPerfProbe.map" ${MAICORD_CLIENT_DELAY_LOAD_OPTIONS})
set_target_properties(MaiCordPerfProbe PROPERTIES CXX_EXTENSIONS OFF MSVC_RUNTIME_LIBRARY "MultiThreaded")
add_dependencies(MaiCordPerfProbe MaiCordPerfAccount)
file(WRITE "${CMAKE_BINARY_DIR}/maicord-test-targets.txt" "MaiCordPerfProbe\nMaiCordPerfAccount\n")
