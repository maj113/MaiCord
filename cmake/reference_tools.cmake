# Opt-in DirectWrite/Direct2D oracles that compare the native text, font and
# COLR paint engines with Windows (tools/font_reference, tools/colr_reference,
# tools/unicode). None of them ships; build one explicitly after configuring
# the Release cache with -DMAICORD_BUILD_FONT_REFERENCE=ON.
option(MAICORD_BUILD_FONT_REFERENCE "Build the DirectWrite/Direct2D reference oracles" OFF)
if(NOT MAICORD_BUILD_FONT_REFERENCE)
    return()
endif()
get_property(maicord_targets_before DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)

set(maicord_reference_libs maicord_core dwrite kernel32)
set(maicord_plan_reference MAICORD_FEATURE_PLAN_REFERENCE=1 MAICORD_PLACEMENT_REFERENCE=1)
maicord_program(MaiCordFontMatchingReference ENTRY maicord_font_matching_reference_entry
    SOURCES tools/font_reference/font_matching_reference.cpp LIBS ${maicord_reference_libs})
maicord_program(MaiCordBidiStyleReference ENTRY maicord_bidi_style_reference_entry
    SOURCES tools/font_reference/bidi_style_reference.cpp LIBS ${maicord_reference_libs}
    DEFINES ${maicord_plan_reference})
maicord_program(MaiCordVisibilityReference ENTRY maicord_visibility_reference_entry
    SOURCES tools/font_reference/visibility_reference.cpp LIBS ${maicord_reference_libs})
maicord_program(MaiCordScriptCutReference ENTRY maicord_script_cut_reference_entry
    SOURCES tools/font_reference/script_cut_reference.cpp LIBS maicord_core maicord_ui dwrite kernel32
    DEFINES ${maicord_plan_reference})
maicord_program(MaiCordFontFallbackReference ENTRY maicord_font_fallback_reference_entry
    SOURCES tools/font_reference/font_fallback_reference.cpp LIBS ${maicord_reference_libs})
# The GSUB/GPOS execution oracle, built once per observation mode.
set(maicord_gsub_reference ENTRY maicord_gsub_execution_reference_entry
    SOURCES tools/font_reference/gsub_execution_reference.cpp LIBS ${maicord_reference_libs})
maicord_program(MaiCordGsubExecutionReference ${maicord_gsub_reference})
maicord_program(MaiCordGposExecutionReference ${maicord_gsub_reference} DEFINES MAICORD_PLACEMENT_REFERENCE=1)
maicord_program(MaiCordFeaturePlanReference ${maicord_gsub_reference} DEFINES ${maicord_plan_reference})
maicord_program(MaiCordEmojiLineReference ${maicord_gsub_reference}
    DEFINES ${maicord_plan_reference} MAICORD_FORCED_FACE_REFERENCE=1)
maicord_program(MaiCordEmojiItemsReference ${maicord_gsub_reference}
    DEFINES ${maicord_plan_reference} MAICORD_FORCED_FACE_REFERENCE=1 MAICORD_ITEMIZED_LAYOUT_REFERENCE=1)
maicord_program(MaiCordBidiReference ENTRY maicord_bidi_reference_entry
    SOURCES tools/unicode/bidi_reference.cpp LIBS maicord_core maicord_unicode kernel32 dwrite)
maicord_program(MaiCordLineBreakReference ENTRY maicord_line_reference_entry
    SOURCES tools/unicode/line_reference.cpp LIBS maicord_core maicord_unicode dwrite kernel32)
maicord_program(MaiCordFontReference ENTRY maicord_font_reference_entry
    SOURCES tools/font_reference/main.cpp tools/font_reference/program_oracle.cpp
        tools/font_reference/dwrite_text_system.cpp tools/colr_reference/dwrite_paint_builder.cpp
    LIBS maicord_core maicord_font maicord_text maicord_paint_program maicord_renderer kernel32 dwrite)
# The real application profile, style, emoji and fallback path without a
# window or graphics device (tools/font_reference/app_text_boundary.py).
maicord_program(MaiCordAppTextBoundary ENTRY maicord_app_text_boundary_entry
    SOURCES tools/font_reference/app_text_boundary.cpp tools/font_reference/dwrite_text_system.cpp
        src/core/log.cpp
    LIBS maicord_core maicord_text maicord_ui maicord_ui_input maicord_model maicord_message_runtime
        maicord_render_core maicord_media_codec maicord_atlas_gpu maicord_renderer kernel32 user32 d3d11 dxgi
        dcomp dwrite)
maicord_copy_fonts(MaiCordAppTextBoundary)

# COLRv1 paint oracle and CPU paint-build benchmark. The CRT and Direct2D
# belong to these QA executables only: they link the shipping archives by path
# and skip the freestanding contract, but keep its definitions so the shared
# headers retain the renderer's conditional record layout.
add_executable(MaiCordColrReference
    tools/colr_reference/probe.cpp tools/colr_reference/native_source.cpp
    tools/colr_reference/dwrite_paint_builder.cpp tools/font_reference/dwrite_text_system.cpp)
add_executable(MaiCordColrBuildBenchmark
    tools/colr_reference/build_benchmark.cpp tools/colr_reference/native_source.cpp
    tools/colr_reference/dwrite_paint_builder.cpp tools/font_reference/dwrite_text_system.cpp)
foreach(target MaiCordColrReference MaiCordColrBuildBenchmark)
    target_include_directories(${target} PRIVATE include "${MAICORD_GENERATED_DIRECTORY}")
    target_compile_definitions(${target} PRIVATE
        "$<TARGET_PROPERTY:maicord_compile_contract,INTERFACE_COMPILE_DEFINITIONS>")
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_options(${target} PRIVATE /GR- /utf-8 /fp:fast /arch:AVX)
    foreach(library maicord_renderer maicord_text maicord_paint_program maicord_font maicord_render_core
            maicord_core maicord_unicode maicord_ui maicord_media_core)
        target_link_libraries(${target} PRIVATE "$<TARGET_FILE:${library}>")
        add_dependencies(${target} ${library})
    endforeach()
    target_link_libraries(${target} PRIVATE kernel32 gdi32 dwrite d3d11 dxgi d2d1 advapi32 ntdll)
endforeach()
# Full-corpus native/Windows paint-program equality pass (--program-work).
maicord_program(MaiCordNativeColrCost ENTRY maicord_colr_cost_entry
    SOURCES tests/native_colr_cost_test.cpp tools/colr_reference/native_source.cpp
        tools/colr_reference/dwrite_paint_builder.cpp tools/font_reference/dwrite_text_system.cpp
    LIBS maicord_core maicord_font maicord_text maicord_paint_program maicord_renderer kernel32 dwrite d3d11
    INCLUDES "${MAICORD_GENERATED_DIRECTORY}")

# The oracles call DirectWrite, Direct2D and Direct3D through the SDK's own
# headers (see MAICORD_SDK_INCLUDES in cmake/build_contract.cmake).
get_property(maicord_targets_after DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
list(REMOVE_ITEM maicord_targets_after ${maicord_targets_before})
set_property(TARGET ${maicord_targets_after} PROPERTY MAICORD_SDK_INCLUDES ON)
