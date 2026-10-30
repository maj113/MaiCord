# Build-time generated inputs: packed UI data, compiled shaders, the
# executable icon and the notification sound resource. Everything is written
# under ${MAICORD_GENERATED_DIRECTORY} in the build directory; the libraries
# that consume an output list it as a source (see libraries.cmake).

find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
find_program(MAICORD_FXC_EXECUTABLE fxc HINTS "$ENV{WindowsSdkVerBinPath}/x64" REQUIRED)
set(MAICORD_GENERATED_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MAICORD_GENERATED_DIRECTORY}")

# Preserve the canonical emoji catalog; emit the compact data in the build tree.
# The catalog and shortcode name/glyph pools ship as one zlib stream that
# src/ui/emoji_catalog.cpp inflates on first use (lengths checked at compile time).
set(MAICORD_EMOJI_CATALOG_INCLUDE "${MAICORD_GENERATED_DIRECTORY}/unicode_emoji_16_optimized.inc")
add_custom_command(
    OUTPUT "${MAICORD_EMOJI_CATALOG_INCLUDE}"
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_unicode_emoji_catalog.py"
        --canonical-include
        --shortcode-include "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/emoji_shortcodes.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/unicode_emoji_16.inc"
        "${MAICORD_EMOJI_CATALOG_INCLUDE}"
    DEPENDS tools/generate_unicode_emoji_catalog.py src/ui/unicode_emoji_16.inc src/ui/emoji_shortcodes.inc
    VERBATIM)
set(MAICORD_EMOJI_POOL_INCLUDE "${MAICORD_GENERATED_DIRECTORY}/emoji_pool_pack.inc")
# Each generated include #embeds its byte arrays from .bin files written beside it.
set(MAICORD_EMOJI_POOL_PACKED "${MAICORD_GENERATED_DIRECTORY}/emoji_pool_packed.bin")
add_custom_command(
    OUTPUT "${MAICORD_EMOJI_POOL_INCLUDE}" "${MAICORD_EMOJI_POOL_PACKED}"
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_emoji_pools.py" "${MAICORD_EMOJI_POOL_INCLUDE}"
        "${MAICORD_EMOJI_CATALOG_INCLUDE}" "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/emoji_shortcodes.inc"
    DEPENDS tools/pack_emoji_pools.py tools/generate_unicode_emoji_catalog.py tools/font_reference/embed_fixture.py
        "${MAICORD_EMOJI_CATALOG_INCLUDE}" src/ui/emoji_shortcodes.inc
    VERBATIM)
add_custom_target(maicord_emoji_catalog_data
    DEPENDS "${MAICORD_EMOJI_CATALOG_INCLUDE}" "${MAICORD_EMOJI_POOL_INCLUDE}")

# The icon manifest (its order defines UiIcon) and its vendored SVG sources.
file(GLOB MAICORD_ICON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lucide/icons/*.svg"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/original/*.svg")
set(MAICORD_ICON_PACK_INCLUDE "${MAICORD_GENERATED_DIRECTORY}/ui_icon_pack.inc")
set(MAICORD_ICON_PACK_DATA "${MAICORD_GENERATED_DIRECTORY}/ui_icon_pack_data.bin")
set(MAICORD_ICON_PACK_PACKED "${MAICORD_GENERATED_DIRECTORY}/ui_icon_pack_packed.bin")
add_custom_command(
    OUTPUT "${MAICORD_ICON_PACK_INCLUDE}" "${MAICORD_ICON_PACK_DATA}" "${MAICORD_ICON_PACK_PACKED}"
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_icon_pack.py" build
        --manifest "${CMAKE_CURRENT_SOURCE_DIR}/include/maicord/ui/icon_ids.inc"
        --lucide "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lucide/icons"
        --original "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/original"
        --output "${MAICORD_ICON_PACK_INCLUDE}"
    DEPENDS tools/generate_icon_pack.py tools/font_reference/embed_fixture.py include/maicord/ui/icon_ids.inc
        ${MAICORD_ICON_SOURCES}
    VERBATIM)
add_custom_target(maicord_icon_pack_data DEPENDS "${MAICORD_ICON_PACK_INCLUDE}")

# Regional flags ship as compact records deflate-compressed in groups; the
# generator packs the committed Twemoji sources and the account module
# inflates a group and rebuilds a flag's SVG text on demand.
file(GLOB MAICORD_REGIONAL_FLAG_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/regional_flags/*.svg")
set(MAICORD_REGIONAL_FLAG_BUNDLE "${MAICORD_GENERATED_DIRECTORY}/regional_flag_bundle.inc")
set(MAICORD_REGIONAL_FLAG_PACKED "${MAICORD_GENERATED_DIRECTORY}/regional_flag_packed.bin")
add_custom_command(
    OUTPUT "${MAICORD_REGIONAL_FLAG_BUNDLE}" "${MAICORD_REGIONAL_FLAG_PACKED}"
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_regional_flag_bundle.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/regional_flags" "${MAICORD_REGIONAL_FLAG_BUNDLE}"
    DEPENDS tools/generate_regional_flag_bundle.py tools/font_reference/embed_fixture.py
        ${MAICORD_REGIONAL_FLAG_SOURCES}
    VERBATIM)
add_custom_target(maicord_regional_flag_data DEPENDS "${MAICORD_REGIONAL_FLAG_BUNDLE}")

set(MAICORD_NOTIFICATION_SOUND_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds")
set(MAICORD_NOTIFICATION_SOUND_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/notification_sounds.rc")
configure_file(config/notification_sounds.rc.in "${MAICORD_NOTIFICATION_SOUND_RESOURCE}" @ONLY)
set_source_files_properties("${MAICORD_NOTIFICATION_SOUND_RESOURCE}" PROPERTIES OBJECT_DEPENDS
    "${MAICORD_NOTIFICATION_SOUND_DIRECTORY}/message-alert.mcp;${MAICORD_NOTIFICATION_SOUND_DIRECTORY}/focused-message-alert.mcp")

# Executable icon: the tray mark in default theme colours, PNG-compressed.
set(MAICORD_APP_ICON "${MAICORD_GENERATED_DIRECTORY}/maicord.ico")
add_custom_command(
    OUTPUT "${MAICORD_APP_ICON}"
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_app_icon.py"
        --output "${MAICORD_APP_ICON}"
    DEPENDS tools/generate_app_icon.py
    VERBATIM)
# Its own target: a custom command listed among MaiCord's sources would wait
# for every library MaiCord links, and every MaiCord object would wait for it.
add_custom_target(maicord_app_icon DEPENDS "${MAICORD_APP_ICON}")
set_source_files_properties(config/maicord.rc PROPERTIES
    OBJECT_DEPENDS "${MAICORD_APP_ICON}"
    COMPILE_OPTIONS "/I${MAICORD_GENERATED_DIRECTORY}")

# maicord_shader(<variable> <header> <profile> <entry> <symbol> <source> [PACK <name>] [<define>...])
# Compiles <source> with fxc into generated/<header>, a byte array named
# <symbol>, and stores the header path in <variable>. PACK adds it, in call
# order, to the shipped shader pack under <name>.
function(maicord_shader variable header profile entry symbol source)
    cmake_parse_arguments(PARSE_ARGV 6 ARG "" "PACK" "")
    set(output "${MAICORD_GENERATED_DIRECTORY}/${header}")
    set(defines)
    foreach(define IN LISTS ARG_UNPARSED_ARGUMENTS)
        list(APPEND defines /D ${define})
    endforeach()
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${MAICORD_FXC_EXECUTABLE}"
            /nologo /T ${profile} /E ${entry} ${defines} /O3 /Ges /WX /Zpr
            /Qstrip_debug /Qstrip_reflect /Qstrip_priv
            /Fh "${output}" /Vn ${symbol}
            "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        DEPENDS ${source}
        VERBATIM)
    set(${variable} "${output}" PARENT_SCOPE)
    if(ARG_PACK)
        set(MAICORD_PACKED_SHADERS ${MAICORD_PACKED_SHADERS} "${ARG_PACK}=${output}" PARENT_SCOPE)
        set(MAICORD_PACKED_SHADER_HEADERS ${MAICORD_PACKED_SHADER_HEADERS} "${output}" PARENT_SCOPE)
    endif()
endfunction()

# Every shipped shader is deflate-packed (one zlib stream each; the wide COLR
# variant is a sparse byte patch on the normal COLR stream) and inflated into
# exact-size heap scratch just before its D3D11 Create* call. The first-frame UI/glyph/media
# set (~60 KB raw, ~13 KB packed) inflates in well under 0.1 ms at startup.
set(MAICORD_PACKED_SHADERS)
set(MAICORD_PACKED_SHADER_HEADERS)
set(maicord_shaders src/render/shaders)
maicord_shader(MAICORD_UI_VERTEX_HEADER maicord_ui_vs.hpp vs_4_0 ui_vertex g_maicord_ui_vertex_shader
    ${maicord_shaders}/ui.hlsl PACK ui_vertex)
maicord_shader(MAICORD_UI_PIXEL_HEADER maicord_ui_ps.hpp ps_4_0 ui_pixel g_maicord_ui_pixel_shader
    ${maicord_shaders}/ui.hlsl PACK ui_pixel)
maicord_shader(MAICORD_GLYPH_VERTEX_HEADER maicord_glyph_vs.hpp vs_4_0 glyph_vertex g_maicord_glyph_vertex_shader
    ${maicord_shaders}/glyph_draw.hlsl PACK glyph_vertex)
maicord_shader(MAICORD_GLYPH_PIXEL_HEADER maicord_glyph_ps.hpp ps_4_0 glyph_pixel g_maicord_glyph_pixel_shader
    ${maicord_shaders}/glyph_draw.hlsl PACK glyph_pixel)
maicord_shader(MAICORD_MEDIA_VERTEX_HEADER maicord_media_vs.hpp vs_4_0 media_vertex g_maicord_media_vertex_shader
    ${maicord_shaders}/media_draw.hlsl PACK media_vertex)
maicord_shader(MAICORD_MEDIA_PIXEL_HEADER maicord_media_ps.hpp ps_4_0 media_pixel g_maicord_media_pixel_shader
    ${maicord_shaders}/media_draw.hlsl PACK media_pixel)
maicord_shader(MAICORD_GLYPH_COMPUTE_HEADER maicord_glyph_cs.hpp cs_5_0 glyph_coverage g_maicord_glyph_compute_shader
    ${maicord_shaders}/glyph_atlas.hlsl PACK glyph_compute)
maicord_shader(MAICORD_GLYPH_CULLED_COMPUTE_HEADER maicord_glyph_culled_cs.hpp cs_5_0 glyph_coverage
    g_maicord_glyph_culled_compute_shader ${maicord_shaders}/glyph_atlas.hlsl PACK glyph_culled_compute
    GLYPH_TILE_CULLING=1)
maicord_shader(MAICORD_COLR_COMPUTE_HEADER maicord_colr_cs.hpp cs_5_0 colr_paint g_maicord_colr_compute_shader
    ${maicord_shaders}/colr_paint.hlsl PACK colr_compute)
maicord_shader(MAICORD_COLR_WIDE_HEADER maicord_colr_wide_cs.hpp cs_5_0 colr_paint g_maicord_colr_wide_compute_shader
    ${maicord_shaders}/colr_paint.hlsl PACK colr_wide_compute COLR_STACK_SIZE=64)
maicord_shader(MAICORD_COLR_LAYERS_HEADER maicord_colr_layers_cs.hpp cs_5_0 colr_paint g_maicord_colr_layers_compute_shader
    ${maicord_shaders}/colr_paint.hlsl PACK colr_layers_compute COLR_LAYERS=1)
maicord_shader(MAICORD_PNG_COMPUTE_HEADER maicord_png_cs.hpp cs_5_0 png_expand g_maicord_png_compute_shader
    ${maicord_shaders}/png_atlas.hlsl PACK png_compute)
maicord_shader(MAICORD_VP8_RECONSTRUCT_HEADER maicord_vp8_reconstruct_cs.hpp cs_5_0 vp8_reconstruct
    g_maicord_vp8_reconstruct_shader ${maicord_shaders}/vp8_reconstruct_tiled.hlsl PACK vp8_reconstruct)
maicord_shader(MAICORD_VP8_IDCT_HEADER maicord_vp8_idct_cs.hpp cs_5_0 vp8_inverse_dct g_maicord_vp8_idct_shader
    ${maicord_shaders}/vp8_reconstruct.hlsl PACK vp8_idct)
maicord_shader(MAICORD_VP8_FILTER_HEADER maicord_vp8_filter_cs.hpp cs_5_0 vp8_filter g_maicord_vp8_filter_shader
    ${maicord_shaders}/vp8_filter.hlsl PACK vp8_filter)
maicord_shader(MAICORD_VP8_ATLAS_HEADER maicord_vp8_atlas_cs.hpp cs_5_0 vp8_to_atlas g_maicord_vp8_atlas_shader
    ${maicord_shaders}/vp8_atlas.hlsl PACK vp8_atlas)
maicord_shader(MAICORD_JPEG_IDCT_HEADER maicord_jpeg_idct_cs.hpp cs_5_0 jpeg_idct g_maicord_jpeg_idct_shader
    ${maicord_shaders}/jpeg.hlsl PACK jpeg_idct)
maicord_shader(MAICORD_JPEG_ATLAS_HEADER maicord_jpeg_atlas_cs.hpp cs_5_0 jpeg_to_atlas g_maicord_jpeg_atlas_shader
    ${maicord_shaders}/jpeg.hlsl PACK jpeg_atlas)
maicord_shader(MAICORD_VIDEO_FRAME_HEADER video_frame_cs.hpp cs_5_0 cs_main g_maicord_video_frame_shader
    ${maicord_shaders}/video_frame.hlsl PACK video_frame)
# Background effect pass: canvas vertex shader and the built-in Stars/Comets
# effect, a ps_5_0 program against the public mc.fx@1 constant buffer.
maicord_shader(MAICORD_FX_VERTEX_HEADER maicord_fx_vs.hpp vs_5_0 fx_vertex g_maicord_fx_vertex_shader
    ${maicord_shaders}/background_fx.hlsl PACK fx_vertex FX_VERTEX=1)
maicord_shader(MAICORD_FX_PIXEL_HEADER maicord_fx_ps.hpp ps_5_0 fx_builtin g_maicord_fx_pixel_shader
    ${maicord_shaders}/background_fx.hlsl PACK fx_builtin)
set(MAICORD_SHADER_PACK_INCLUDE "${MAICORD_GENERATED_DIRECTORY}/maicord_shader_pack.inc")
set(MAICORD_SHADER_PACK_PACKED "${MAICORD_GENERATED_DIRECTORY}/maicord_shader_pack.bin")
add_custom_command(
    OUTPUT "${MAICORD_SHADER_PACK_INCLUDE}" "${MAICORD_SHADER_PACK_PACKED}"
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_shader_bytecode.py" "${MAICORD_SHADER_PACK_INCLUDE}"
        ${MAICORD_PACKED_SHADERS}
    DEPENDS tools/pack_shader_bytecode.py tools/font_reference/embed_fixture.py ${MAICORD_PACKED_SHADER_HEADERS}
    VERBATIM)
