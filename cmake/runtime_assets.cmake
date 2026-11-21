# The runtime assets both clients carry beside the executable (included by cmake/client.cmake once MaiCord exists):
# the license texts, and the bundled fonts the renderer reads from fonts/.
set(maicord_license_files
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/Opus-BSD.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/Twemoji-graphics.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/Unicode-3.0.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/ICU-Thai-dictionary.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/PyThaiNLP-corpus.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/gemoji-MIT.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/emojibase-MIT.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/MaiCord-sounds.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/Lucide-ISC.txt")
file(GLOB maicord_runtime_font_files
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.otf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL-*.txt")
# One rule copies the assets next to every executable (they share the runtime output directory): per-target POST_BUILD
# copies raced each other in parallel builds. It runs when an asset changes, beside the compiles, not after each link.
# Ninja creates the output directories.
set(maicord_runtime_asset_outputs "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/licenses/MaiCord-MPL-2.0.txt")
foreach(file IN LISTS maicord_license_files maicord_runtime_font_files)
    cmake_path(GET file FILENAME name)
    cmake_path(GET file PARENT_PATH directory)
    cmake_path(GET directory FILENAME directory)
    list(APPEND maicord_runtime_asset_outputs "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${directory}/${name}")
endforeach()
add_custom_command(OUTPUT ${maicord_runtime_asset_outputs}
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/licenses/MaiCord-MPL-2.0.txt"
    COMMAND ${CMAKE_COMMAND} -E copy ${maicord_license_files} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy ${maicord_runtime_font_files} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" ${maicord_license_files} ${maicord_runtime_font_files}
    COMMENT "Copying the licenses and fonts"
    VERBATIM)
add_custom_target(maicord_runtime_assets DEPENDS ${maicord_runtime_asset_outputs})
# maicord_copy_fonts(<target>...): targets whose run reads the bundled fonts
# next to their executable.
function(maicord_copy_fonts)
    foreach(target IN LISTS ARGN)
        add_dependencies(${target} maicord_runtime_assets)
    endforeach()
endfunction()
maicord_copy_fonts(MaiCord)
