# The runtime assets both clients carry beside the executable (included by cmake/client.cmake and cmake/linux_client.cmake
# once MaiCord exists): the license texts, and the bundled fonts the renderer reads from fonts/.
add_custom_command(TARGET MaiCord POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MaiCord>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus/COPYING"
        "$<TARGET_FILE_DIR:MaiCord>/licenses/Opus-BSD.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:MaiCord>/licenses/MaiCord-MPL-2.0.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/Twemoji-graphics.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/Unicode-3.0.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/ICU-Thai-dictionary.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/PyThaiNLP-corpus.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/gemoji-MIT.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/emojibase-MIT.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/MaiCord-sounds.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/Lucide-ISC.txt"
        "$<TARGET_FILE_DIR:MaiCord>/licenses")
file(GLOB maicord_runtime_font_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.otf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL-*.txt")
# One rule copies the bundled fonts next to every executable (they share the
# runtime output directory); per-target POST_BUILD copies raced each other in
# parallel builds.
set(maicord_runtime_font_outputs "")
foreach(font IN LISTS maicord_runtime_font_files)
    get_filename_component(name "${font}" NAME)
    list(APPEND maicord_runtime_font_outputs "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts/${name}")
endforeach()
add_custom_command(OUTPUT ${maicord_runtime_font_outputs}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${maicord_runtime_font_files}
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts"
    DEPENDS ${maicord_runtime_font_files}
    VERBATIM)
add_custom_target(maicord_runtime_fonts DEPENDS ${maicord_runtime_font_outputs})
# maicord_copy_fonts(<target>...): targets whose run reads the bundled fonts
# next to their executable.
function(maicord_copy_fonts)
    foreach(target IN LISTS ARGN)
        add_dependencies(${target} maicord_runtime_fonts)
    endforeach()
endfunction()
maicord_copy_fonts(MaiCord)
