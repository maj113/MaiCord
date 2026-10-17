# Temporary deterministic tests. tools/build.ps1 -Test configures them in the
# Release cache, builds and runs the suite, removes every test output and
# configures the cache back to shipping-only.
set(MAICORD_TEST_TIMEOUT_SCALE 1 CACHE STRING "Multiplier applied to every per-test timeout (hosted CI runners)")
option(MAICORD_BUILD_TESTS "Enable temporary deterministic test targets" OFF)
set(MAICORD_TEST_PROFILE_NAMES "" CACHE STRING "Selected tests needing isolated profile directories; empty means all tests")
option(MAICORD_TEST_AUDIO_PLAYBACK "Include the native WASAPI Opus playback contract" OFF)
option(MAICORD_TEST_PREBUILT_CLIENT
    "Tests use the MaiCord.exe and MaiCordAccount.dll already in the build directory instead of building them" OFF)
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
    file(REMOVE "${CMAKE_BINARY_DIR}/maicord-test-targets.txt" "${CMAKE_BINARY_DIR}/maicord-test-shards.txt"
                "${CMAKE_BINARY_DIR}/CTestTestfile.cmake")
    unset(MAICORD_TEST_PROFILE_DIRECTORY CACHE)
    unset(MAICORD_TEST_PROFILE_NAMES CACHE)
    return()
endif()

include(cmake/test_support.cmake)
get_property(maicord_production_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
enable_testing()
include(cmake/test_registry.cmake)
maicord_copy_fonts(MaiCordUiProbe MaiCordMessageLinkUiProbe MaiCordComposerUiProbe MaiCordAccountLogoutUiProbe
    MaiCordScopedReadUiProbe MaiCordFontCatalogTests)

# Every test target, the non-/GL library twins, shared objects and PCH
# providers are temporary: tools/build.ps1 builds the ones the selected tests
# need and maintain_build_outputs.ps1 removes their outputs by this manifest.
get_property(maicord_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
list(REMOVE_ITEM maicord_test_targets ${maicord_production_targets})
maicord_add_test_codegen_twins(maicord_test_targets)
maicord_share_test_objects(maicord_test_targets)
maicord_add_test_precompiled_headers(maicord_test_targets)
set_property(TARGET ${maicord_test_targets} PROPERTY EXCLUDE_FROM_ALL TRUE)
list(JOIN maicord_test_targets "\n" maicord_test_target_manifest)
file(WRITE "${CMAKE_BINARY_DIR}/maicord-test-targets.txt" "${maicord_test_target_manifest}\n")
maicord_write_test_shards("${CMAKE_BINARY_DIR}/maicord-test-shards.txt" ${maicord_test_targets})

# Per-test environment: no Python bytecode in the source tree and, for the
# selected tests (all by default), the private profile directories.
get_property(maicord_registered_tests DIRECTORY PROPERTY TESTS)
set(maicord_test_profile_index 0)
foreach(maicord_registered_test IN LISTS maicord_registered_tests)
    set_property(TEST ${maicord_registered_test} APPEND PROPERTY
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    if(MAICORD_TEST_PROFILE_DIRECTORY AND
       (NOT MAICORD_TEST_PROFILE_NAMES OR maicord_registered_test IN_LIST MAICORD_TEST_PROFILE_NAMES))
        math(EXPR maicord_test_profile_index "${maicord_test_profile_index} + 1")
        set(maicord_test_directory "${maicord_test_profile}/${maicord_test_profile_index}")
        file(MAKE_DIRECTORY "${maicord_test_directory}/Temp" "${maicord_test_directory}/Local")
        file(TO_NATIVE_PATH "${maicord_test_directory}/Temp" maicord_test_temp)
        file(TO_NATIVE_PATH "${maicord_test_directory}/Local" maicord_test_local)
        set_property(TEST ${maicord_registered_test} APPEND PROPERTY
            ENVIRONMENT "TEMP=${maicord_test_temp}" "TMP=${maicord_test_temp}")
        get_test_property(${maicord_registered_test} ENVIRONMENT maicord_test_environment)
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

# Longest-first start order (CTest COST, descending), from durations measured
# under the full 16-job suite. Tests that mostly wait on peers
# or deadlines start early because they cost little CPU. The GPU-locked probe
# chain (COST 60) starts immediately and overlaps the CPU matrices (COST 59.xx).
# These independent window/readback probes also need an early start: resize
# paints 64 native WM_SIZE steps; surface motion reads back multiple alpha
# frames. Neither belongs to the serialized native_gpu chain.
set_tests_properties(maicord_resize_stress_probe maicord_surface_motion_ui_probe
    PROPERTIES COST 61)
# The extension driver's independent fixture shards also start early so its
# durable file work overlaps the CPU matrices without a serial final tail.
set(maicord_test_rank 99)
foreach(maicord_long_test
    maicord_native_extensions
    maicord_native_extensions_shard_1 maicord_native_extensions_shard_2
    maicord_native_extensions_shard_3
    maicord_native_bidi_emoji
    maicord_native_directional_editor_cuts
    maicord_update_handoff maicord_account_logout_runtime
    maicord_native_bidi_styles
    maicord_native_mixed_emoji
    maicord_native_style_clusters
    maicord_native_matrix_decorations
    maicord_native_symbols
    maicord_native_directional_edit_cuts
    maicord_notification_audio_faults maicord_native_simulated_styles
    maicord_native_directional_semantic_ranges maicord_core
    maicord_native_matrix_styled maicord_native_matrix_multiline_private_hits
    maicord_native_long_text maicord_native_emoji_dispatch
    maicord_native_long_editor maicord_native_matrix_multiline_hits
    maicord_native_mixed_bidi maicord_native_matrix_ellipsis_hits
    maicord_native_styled_editor maicord_native_direction_tracking
    maicord_native_direction_tracking_default maicord_native_direction_tracking_private
    maicord_native_hebrew_wrapping maicord_native_style_continuity_pixelify)
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
    if("native_gpu" IN_LIST maicord_test_locks)
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
