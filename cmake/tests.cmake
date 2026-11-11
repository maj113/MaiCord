# Temporary deterministic tests of both systems, registered in cmake/test_registry.cmake. tools/build.ps1 -Test
# (Windows) and tools/build_linux.sh --test (Linux) configure them, build and run the suite and remove every test output.
set(MAICORD_TEST_TIMEOUT_SCALE 1 CACHE STRING "Multiplier applied to every per-test timeout (hosted CI runners)")
set(MAICORD_TEST_PROFILE_NAMES "" CACHE STRING "Selected tests needing isolated profile directories; empty means all tests")
option(MAICORD_TEST_AUDIO_PLAYBACK "Include the native WASAPI Opus playback contract" OFF)
option(MAICORD_TEST_PREBUILT_CLIENT
    "Tests use the MaiCord.exe already in the build directory instead of building it" OFF)
# tools/build.ps1 gives every selected test a private TEMP/TMP/LOCALAPPDATA in
# a profile directory it owns inside the build directory.
if(MAICORD_BUILD_TESTS AND MAICORD_TEST_PROFILE_DIRECTORY)
    file(REAL_PATH "${MAICORD_TEST_PROFILE_DIRECTORY}" maicord_test_profile)
    cmake_path(GET maicord_test_profile PARENT_PATH maicord_test_profile_parent)
    cmake_path(GET maicord_test_profile FILENAME maicord_test_profile_name)
    string(LENGTH "${maicord_test_profile_name}" maicord_test_profile_name_length)
    if(NOT maicord_test_profile_parent STREQUAL CMAKE_BINARY_DIR OR
       NOT maicord_test_profile_name_length EQUAL 45 OR
       NOT maicord_test_profile_name MATCHES "^test-profile-[0-9a-f]+$" OR
       NOT EXISTS "${maicord_test_profile}/.maicord-test-profile")
        message(FATAL_ERROR "Tests require an owned profile inside the single Release build.")
    endif()
    file(READ "${maicord_test_profile}/.maicord-test-profile" maicord_test_profile_owner)
    file(TO_CMAKE_PATH "${maicord_test_profile_owner}" maicord_test_profile_owner)
    if(NOT maicord_test_profile_owner STREQUAL CMAKE_SOURCE_DIR)
        message(FATAL_ERROR "The test profile belongs to another source directory.")
    endif()
endif()
if(NOT MAICORD_BUILD_TESTS)
    file(REMOVE "${CMAKE_BINARY_DIR}/maicord-tests.txt" "${CMAKE_BINARY_DIR}/CTestTestfile.cmake")
    unset(MAICORD_TEST_PROFILE_DIRECTORY CACHE)
    unset(MAICORD_TEST_PROFILE_NAMES CACHE)
    return()
endif()

include(cmake/test_support.cmake)
get_property(maicord_production_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
enable_testing()
include(cmake/test_registry.cmake)
maicord_finish_test_bundles()

# The non-/GL library twins, shared objects and test PCH libraries are temporary
# like the tests: the build scripts build the ones the selected tests need
# (maicord-tests.txt) and remove their outputs afterwards.
get_property(maicord_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
list(REMOVE_ITEM maicord_test_targets ${maicord_production_targets})
if(MAICORD_LINUX)
    # The tests' own objects need no bitcode: only the shipping libraries' code goes through the LTO of each test's link.
    # They are compiled without it, and a source several tests compile alike is compiled once (maicord_share_test_objects).
    foreach(target IN LISTS maicord_test_targets)
        get_target_property(type ${target} TYPE)
        if(type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|OBJECT_LIBRARY)$")
            set_property(TARGET ${target} PROPERTY MAICORD_TEST_CODEGEN ON)
        endif()
    endforeach()
    maicord_share_test_objects(maicord_test_targets)
else()
    maicord_copy_fonts(MaiCordUiProbe MaiCordMessageLinkUiProbe MaiCordComposerUiProbe MaiCordAccountLogoutUiProbe
        MaiCordScopedReadUiProbe MaiCordTextUnitTests)
    maicord_add_test_codegen_twins(maicord_test_targets)
    maicord_share_test_objects(maicord_test_targets)
    # The tests compiling the most application sources use the application PCH too.
    maicord_precompile_test_application_sources(maicord_test_targets
        MaiCordUiProbe MaiCordSettingsRuntimeTests MaiCordTests)
    # The compiles of the tests follow the client's, so its LTO link starts early
    # and overlaps them. A client built elsewhere has nothing to wait for.
    if(NOT MAICORD_TEST_PREBUILT_CLIENT)
        maicord_order_tests_after_client(${maicord_test_targets})
    endif()
    # The tests/ sources only drive the code under test (src/, still /O2), so they
    # compile at /Od: several times faster, and nothing measures their own speed.
    # Sources using an application PCH keep its /O2 (clang rejects a mismatch),
    # received_attachment_open.cpp, which crashes unoptimized (its `resolver = {}`
    # temporaries are about 130 KB each), stays optimized, and so does the reference
    # driver of MaiCordNativeAppLineTests, the largest share of the suite's CPU time
    # (its cases ran about 15% faster with it optimized).
    set(maicord_optimized_test_sources "${CMAKE_CURRENT_SOURCE_DIR}/tests/app/media/received_attachment_open.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/text/native_app_line_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/text/native_app_line_scripts.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/text/native_app_line_editing.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/text/app_line_reference.cpp")
    set(maicord_unoptimized_sources)
    foreach(target IN LISTS maicord_test_targets)
        get_target_property(sources ${target} SOURCES)
        foreach(source IN LISTS sources)
            if(source MATCHES "^\\$<" OR NOT source MATCHES "\\.cpp$")
                continue()
            endif()
            set(path "${source}")
            cmake_path(ABSOLUTE_PATH path BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
            get_source_file_property(pch_mode "${path}" MAICORD_APPLICATION_PCH_MODE)
            if(path MATCHES "^${CMAKE_CURRENT_SOURCE_DIR}/tests/" AND NOT pch_mode
                    AND NOT path IN_LIST maicord_optimized_test_sources AND NOT path IN_LIST maicord_unoptimized_sources)
                set_property(SOURCE "${path}" APPEND PROPERTY COMPILE_OPTIONS /Od)
                list(APPEND maicord_unoptimized_sources "${path}")
            endif()
        endforeach()
    endforeach()
endif()
set_property(TARGET ${maicord_test_targets} PROPERTY EXCLUDE_FROM_ALL TRUE)

# Per-test environment: no Python bytecode in the source tree and private
# profile directories. Windows: for the selected tests (all by default), the
# TEMP, TMP and LOCALAPPDATA of a profile tools/build.ps1 owns. Linux: the data,
# state and cache directories (XDG_DATA_HOME, XDG_STATE_HOME, XDG_CACHE_HOME;
# the log lives under the state directory, D19), and no Secret Service, so that
# nothing a test writes reaches the user's own files or keyring (a test that asks
# for the Secret Service, MAICORD_SECRET_SERVICE=auto, talks to the mock service
# of tools/test_dbus.sh).
get_property(maicord_registered_tests DIRECTORY PROPERTY TESTS)
maicord_write_test_manifest("${CMAKE_BINARY_DIR}/maicord-tests.txt" ${maicord_registered_tests})
set(maicord_test_profile_index 0)
foreach(maicord_registered_test IN LISTS maicord_registered_tests)
    set_property(TEST ${maicord_registered_test} APPEND PROPERTY
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    get_test_property(${maicord_registered_test} ENVIRONMENT maicord_test_environment)
    if(MAICORD_LINUX)
        set(maicord_test_directory "${CMAKE_BINARY_DIR}/test-profile/${maicord_registered_test}")
        foreach(maicord_variable IN ITEMS "XDG_DATA_HOME=${maicord_test_directory}"
                "XDG_STATE_HOME=${maicord_test_directory}/state" "XDG_CACHE_HOME=${maicord_test_directory}/cache"
                MAICORD_SECRET_SERVICE=off)
            string(REGEX REPLACE "=.*" "=" maicord_variable_name "${maicord_variable}")
            if(NOT maicord_test_environment MATCHES "(^|;)${maicord_variable_name}")
                set_property(TEST ${maicord_registered_test} APPEND PROPERTY ENVIRONMENT "${maicord_variable}")
            endif()
        endforeach()
    elseif(MAICORD_TEST_PROFILE_DIRECTORY AND
           (NOT MAICORD_TEST_PROFILE_NAMES OR maicord_registered_test IN_LIST MAICORD_TEST_PROFILE_NAMES))
        math(EXPR maicord_test_profile_index "${maicord_test_profile_index} + 1")
        set(maicord_test_directory "${maicord_test_profile}/${maicord_test_profile_index}")
        file(MAKE_DIRECTORY "${maicord_test_directory}/Temp" "${maicord_test_directory}/Local")
        file(TO_NATIVE_PATH "${maicord_test_directory}/Temp" maicord_test_temp)
        file(TO_NATIVE_PATH "${maicord_test_directory}/Local" maicord_test_local)
        set_property(TEST ${maicord_registered_test} APPEND PROPERTY
            ENVIRONMENT "TEMP=${maicord_test_temp}" "TMP=${maicord_test_temp}")
        if(NOT maicord_test_environment MATCHES "(^|;)LOCALAPPDATA=")
            set_property(TEST ${maicord_registered_test} APPEND PROPERTY
                ENVIRONMENT "LOCALAPPDATA=${maicord_test_local}")
        endif()
    endif()
    # Every test finishes within 20 s; slower hosted runners scale each limit.
    get_test_property(${maicord_registered_test} TIMEOUT maicord_test_timeout)
    if(NOT maicord_test_timeout OR maicord_test_timeout GREATER 20)
        set_property(TEST ${maicord_registered_test} PROPERTY TIMEOUT 20)
        set(maicord_test_timeout 20)
    endif()
    if(MAICORD_TEST_TIMEOUT_SCALE GREATER 1 AND maicord_test_timeout MATCHES "^[0-9]+$")
        math(EXPR maicord_test_timeout "${maicord_test_timeout} * ${MAICORD_TEST_TIMEOUT_SCALE}")
        set_property(TEST ${maicord_registered_test} PROPERTY TIMEOUT ${maicord_test_timeout})
    endif()
endforeach()

# Longest-first start order of the Windows suite (CTest COST, descending), from
# durations measured under the full 16-job suite. The GPU-locked probe chains
# (COST 60) start immediately and overlap the CPU-bound tests ranked below them
# (COST 59.xx). These independent window/readback probes also need an early
# start: resize paints 64 native WM_SIZE steps; surface motion reads back
# multiple alpha frames. Neither belongs to the serialized native_gpu chains.
if(MAICORD_LINUX)
    return()
endif()
set_tests_properties(maicord_resize_stress_probe maicord_surface_motion_ui_probe
    PROPERTIES COST 61)
# Every other test that took 0.6 s or more, longest first; a long test that
# starts late forms the suite's tail.
set(maicord_test_rank 99)
foreach(maicord_long_test
    maicord_stream_encoder_gpu_h264 maicord_stream_encoder_gpu_h265 maicord_stream_encoder_gpu_av1
    maicord_go_live_loopback
    maicord_profile_media_sequence_ui_probe maicord_plugin_dev maicord_core maicord_native_arabic_run
    maicord_native_arabic_plan maicord_resize_text_ui_probe maicord_network_loop
    maicord_voice_setup maicord_media_memory_probe maicord_native_matrix_decorations
    maicord_browser_handoff_exchange maicord_unified_picker_ui_probe maicord_store_ui_probe
    maicord_remote_auth_cancellation maicord_guild_channel_uniqueness maicord_build_output_guard
    maicord_glyph_color_control_1 maicord_glyph_easing maicord_native_simulated_styles maicord_native_mixed_emoji
    maicord_glyph_stage_control_2 maicord_glyph_stage maicord_glyph_color_control_2
    maicord_native_directional_semantic_ranges maicord_forum_post_ui_probe maicord_glyph_color
    maicord_plugin_binding maicord_glyph_stage_control_1 maicord_native_directional_editor_cuts
    maicord_update_client maicord_pane_glyph_stress_ui_probe)
    if(maicord_long_test IN_LIST maicord_registered_tests)
        set_property(TEST ${maicord_long_test} PROPERTY COST 59.${maicord_test_rank})
    endif()
    math(EXPR maicord_test_rank "${maicord_test_rank} - 1")
endforeach()
# Keep the complete serialized GPU chain ahead of the CPU matrices,
# including reference tests and newly registered probes. Independent display,
# GPU and text-render probes follow the ranked CPU jobs but precede the short
# font/process tests; their device/window setup must not form the final tail.
foreach(maicord_registered_test IN LISTS maicord_registered_tests)
    get_property(maicord_test_locks TEST ${maicord_registered_test} PROPERTY RESOURCE_LOCK)
    if(maicord_test_locks MATCHES "(^|;)native_gpu(_[bc])?(;|$)")
        set_property(TEST ${maicord_registered_test} PROPERTY COST 60)
    else()
        get_property(maicord_test_labels TEST ${maicord_registered_test} PROPERTY LABELS)
        if("display" IN_LIST maicord_test_labels OR "gpu" IN_LIST maicord_test_labels OR
           "native-text-render" IN_LIST maicord_test_labels)
            get_property(maicord_test_cost TEST ${maicord_registered_test} PROPERTY COST)
            if(NOT maicord_test_cost OR maicord_test_cost LESS 58)
                set_property(TEST ${maicord_registered_test} PROPERTY COST 58)
            endif()
        endif()
    endif()
endforeach()
# The pacing probe holds every GPU chain (nothing else of them draws while it measures a window's frame pacing): it
# starts first, so that it neither waits for the chains to drain, which makes it the tail of the suite, nor holds them
# up for more than its own 0.5 s.
set_property(TEST maicord_post_resize_pacing_probe PROPERTY COST 62)
