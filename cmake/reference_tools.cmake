# Opt-in DirectWrite oracles that record what Windows does for the generators in
# tools/font_reference (the data compiled into the client under src/**/generated and
# the committed tests/generated fixtures). None of them ships; build one explicitly
# after configuring the Release cache with -DMAICORD_BUILD_FONT_REFERENCE=ON.
option(MAICORD_BUILD_FONT_REFERENCE "Build the DirectWrite reference oracles" OFF)
if(NOT MAICORD_BUILD_FONT_REFERENCE)
    return()
endif()
get_property(maicord_targets_before DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)

set(maicord_reference_libs maicord_core dwrite kernel32)
set(maicord_plan_reference MAICORD_FEATURE_PLAN_REFERENCE=1 MAICORD_PLACEMENT_REFERENCE=1)
maicord_program(MaiCordFontMatchingReference ENTRY maicord_font_matching_reference_entry
    SOURCES tools/font_reference/font_matching_reference.cpp LIBS ${maicord_reference_libs})
maicord_program(MaiCordVisibilityReference ENTRY maicord_visibility_reference_entry
    SOURCES tools/font_reference/visibility_reference.cpp LIBS ${maicord_reference_libs})
maicord_program(MaiCordFontFallbackReference ENTRY maicord_font_fallback_reference_entry
    SOURCES tools/font_reference/font_fallback_reference.cpp LIBS ${maicord_reference_libs})
# The GSUB/GPOS execution oracle, built once per observation mode.
set(maicord_gsub_reference ENTRY maicord_gsub_execution_reference_entry
    SOURCES tools/font_reference/gsub_execution_reference.cpp LIBS ${maicord_reference_libs})
maicord_program(MaiCordGsubExecutionReference ${maicord_gsub_reference})
maicord_program(MaiCordFeaturePlanReference ${maicord_gsub_reference} DEFINES ${maicord_plan_reference})
maicord_program(MaiCordEmojiLineReference ${maicord_gsub_reference}
    DEFINES ${maicord_plan_reference} MAICORD_FORCED_FACE_REFERENCE=1)
maicord_program(MaiCordEmojiItemsReference ${maicord_gsub_reference}
    DEFINES ${maicord_plan_reference} MAICORD_FORCED_FACE_REFERENCE=1 MAICORD_ITEMIZED_LAYOUT_REFERENCE=1)
maicord_program(MaiCordFontReference ENTRY maicord_font_reference_entry
    SOURCES tools/font_reference/main.cpp tools/font_reference/program_oracle.cpp
        tools/font_reference/dwrite_outline_reference.cpp tools/colr_reference/dwrite_paint_builder.cpp
    LIBS maicord_core maicord_font maicord_text maicord_paint_program maicord_renderer kernel32 dwrite)

# The oracles call DirectWrite through the SDK's own headers (see MAICORD_SDK_INCLUDES in
# cmake/build_contract.cmake).
get_property(maicord_targets_after DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
list(REMOVE_ITEM maicord_targets_after ${maicord_targets_before})
set_property(TARGET ${maicord_targets_after} PROPERTY MAICORD_SDK_INCLUDES ON)
