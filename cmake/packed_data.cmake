# The packed data both systems' clients embed, generated into ${MAICORD_GENERATED_DIRECTORY} (cmake/generated_sources.cmake
# and cmake/linux_generated_sources.cmake include this file, set maicord_generated_include_options to the compiler's
# way of adding that directory to the include path, and wire what they consume themselves).
#
# Each include #embeds its byte arrays from .bin files written beside it, and ships the deflate streams of
# tools/deflate_cache.txt (tools/optimal_deflate.py).

find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
set(MAICORD_GENERATED_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MAICORD_GENERATED_DIRECTORY}")

# Preserve the canonical emoji catalog; emit the compact data in the build tree.
# The catalog and shortcode name/glyph pools and both entry tables ship as one zlib stream that
# src/ui/text/emoji_catalog.cpp inflates on first use (lengths checked at compile time).
set(MAICORD_EMOJI_CATALOG_INCLUDE "${MAICORD_GENERATED_DIRECTORY}/unicode_emoji_16_optimized.inc")
add_custom_command(
    OUTPUT "${MAICORD_EMOJI_CATALOG_INCLUDE}"
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_unicode_emoji_catalog.py"
        --canonical-include
        --shortcode-include "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/text/emoji_shortcodes.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/text/unicode_emoji_16.inc"
        "${MAICORD_EMOJI_CATALOG_INCLUDE}"
    DEPENDS tools/generate_unicode_emoji_catalog.py src/ui/text/unicode_emoji_16.inc src/ui/text/emoji_shortcodes.inc
    VERBATIM)
set(MAICORD_EMOJI_POOL_INCLUDE "${MAICORD_GENERATED_DIRECTORY}/emoji_pool_pack.inc")
set(MAICORD_EMOJI_POOL_PACKED "${MAICORD_GENERATED_DIRECTORY}/emoji_pool_packed.bin")
add_custom_command(
    OUTPUT "${MAICORD_EMOJI_POOL_INCLUDE}" "${MAICORD_EMOJI_POOL_PACKED}"
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_emoji_pools.py" "${MAICORD_EMOJI_POOL_INCLUDE}"
        "${MAICORD_EMOJI_CATALOG_INCLUDE}" "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/text/emoji_shortcodes.inc"
    DEPENDS tools/pack_emoji_pools.py tools/generate_unicode_emoji_catalog.py tools/font_reference/embed_fixture.py
        tools/optimal_deflate.py tools/deflate_cache.txt "${MAICORD_EMOJI_CATALOG_INCLUDE}" src/ui/text/emoji_shortcodes.inc
    VERBATIM)
# The libraries that consume the emoji catalog wait for it through this target.
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
        --manifest "${CMAKE_CURRENT_SOURCE_DIR}/include/maicord/ui/widgets/icon_ids.inc"
        --lucide "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lucide/icons"
        --original "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/original"
        --output "${MAICORD_ICON_PACK_INCLUDE}"
    DEPENDS tools/generate_icon_pack.py tools/optimal_deflate.py tools/deflate_cache.txt tools/font_reference/embed_fixture.py
        include/maicord/ui/widgets/icon_ids.inc
        ${MAICORD_ICON_SOURCES}
    VERBATIM)
# The libraries that consume the icon pack wait for it through this target.
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
    DEPENDS tools/generate_regional_flag_bundle.py tools/optimal_deflate.py tools/deflate_cache.txt
        tools/font_reference/embed_fixture.py
        ${MAICORD_REGIONAL_FLAG_SOURCES}
    VERBATIM)

# The settings catalog (src/app/settings/catalog_data.inc) ships as a zlib stream that catalog.cpp inflates on first
# use; tests/app/settings/catalog_pack.cpp checks the decoded catalog against the data file. The source property
# applies to every target that compiles catalog.cpp (the client and its tests).
set(MAICORD_SETTINGS_CATALOG_PACK "${MAICORD_GENERATED_DIRECTORY}/settings_catalog_pack.inc")
add_custom_command(
    OUTPUT "${MAICORD_SETTINGS_CATALOG_PACK}"
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_settings_catalog.py" "${MAICORD_SETTINGS_CATALOG_PACK}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/app/settings/catalog_data.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/app/settings/catalog.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/maicord/ui/settings/scene.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/maicord/ui/settings/schema.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/maicord/ui/widgets/icon_ids.inc"
    DEPENDS tools/pack_settings_catalog.py tools/optimal_deflate.py tools/font_reference/embed_fixture.py
        src/app/settings/catalog_data.inc
        src/app/settings/catalog.hpp include/maicord/ui/settings/scene.hpp include/maicord/ui/settings/schema.hpp
        include/maicord/ui/widgets/icon_ids.inc
    VERBATIM)

# The permission grid (src/model/guild/permission_grid_data.inc) and the built-in games (src/app/account/game_data.inc)
# ship as zlib streams their owners inflate on first use; tests/model/guild/settings_model.cpp and
# tests/ui/widgets/presence_wave.cpp check the decoded tables against the data files.
set(MAICORD_PERMISSION_GRID_PACK "${MAICORD_GENERATED_DIRECTORY}/permission_grid_pack.inc")
set(MAICORD_GAME_PACK "${MAICORD_GENERATED_DIRECTORY}/game_pack.inc")
add_custom_command(
    OUTPUT "${MAICORD_PERMISSION_GRID_PACK}" "${MAICORD_GAME_PACK}"
    BYPRODUCTS "${MAICORD_GENERATED_DIRECTORY}/permission_grid_packed.bin" "${MAICORD_GENERATED_DIRECTORY}/game_packed.bin"
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_tables.py" "${MAICORD_GENERATED_DIRECTORY}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/model/guild/permission_grid_data.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/maicord/model/guild/permission_grid.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/maicord/model/guild/permission_bits.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/app/account/game_data.inc"
    DEPENDS tools/pack_tables.py tools/pack_settings_catalog.py tools/optimal_deflate.py
        tools/font_reference/embed_fixture.py
        src/model/guild/permission_grid_data.inc include/maicord/model/guild/permission_grid.hpp
        include/maicord/model/guild/permission_bits.hpp src/app/account/game_data.inc
    VERBATIM)

# Cold text that is large and repetitive ships as a zlib stream its owner inflates on first use
# (tools/pack_text_blobs.py): the built-in slash-command index.
set(MAICORD_BUILTIN_COMMANDS_PACK "${MAICORD_GENERATED_DIRECTORY}/builtin_commands_pack.inc")
add_custom_command(
    OUTPUT "${MAICORD_BUILTIN_COMMANDS_PACK}"
    BYPRODUCTS "${MAICORD_GENERATED_DIRECTORY}/builtin_commands_packed.bin"
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_text_blobs.py" "${MAICORD_GENERATED_DIRECTORY}"
        "BuiltinCommands=${CMAKE_CURRENT_SOURCE_DIR}/src/app/chat/composer/builtin_commands.json"
    DEPENDS tools/pack_text_blobs.py tools/optimal_deflate.py tools/font_reference/embed_fixture.py
        src/app/chat/composer/builtin_commands.json
    VERBATIM)

# The executable's icon (the tray mark on the violet brand tile, PNG-compressed): the file the Windows resource
# compiler takes and src/core/linux/resource.cpp #embeds.
set(MAICORD_APP_ICON "${MAICORD_GENERATED_DIRECTORY}/maicord.ico")
add_custom_command(
    OUTPUT "${MAICORD_APP_ICON}"
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_app_icon.py"
        --output "${MAICORD_APP_ICON}"
    DEPENDS tools/generate_app_icon.py
    VERBATIM)

# maicord_embed_pack(<source> <pack>...): <source> includes the packs, so they are order-only inputs of its object and
# the generated directory joins its include path.
function(maicord_embed_pack source)
    set_source_files_properties(${source} PROPERTIES OBJECT_DEPENDS "${ARGN}"
        COMPILE_OPTIONS "${maicord_generated_include_options}")
endfunction()
maicord_embed_pack(src/app/settings/catalog.cpp "${MAICORD_SETTINGS_CATALOG_PACK}")
maicord_embed_pack(src/model/guild/permission_grid.cpp "${MAICORD_PERMISSION_GRID_PACK}")
maicord_embed_pack(src/app/account/game_detection.cpp "${MAICORD_GAME_PACK}")
maicord_embed_pack(src/app/chat/composer/builtin_commands.cpp "${MAICORD_BUILTIN_COMMANDS_PACK}")
