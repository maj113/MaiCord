# The shipping client of the Linux build: MaiCord, the application's shell over the libraries of the other
# cmake/linux_*.cmake files, with the Wayland window backend (the null backend, maicord_window_null, is the tests') and
# the runtime assets beside it as cmake/client.cmake puts them beside MaiCord.exe. tools/build_linux.sh audits it.

# The self-updater's and the plugin catalog's identity (shared with the Windows client).
include(cmake/update_config.cmake)

# The shell: every unit of src/app/shell (the Windows build lists them one by one in cmake/client.cmake; the entry and
# the window glue are src/app/linux).
file(GLOB MAICORD_SHELL_SOURCES CONFIGURE_DEPENDS RELATIVE "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/app/shell/*.cpp")

maicord_program(MaiCord ENTRY maicord_entry
    SOURCES ${MAICORD_SHELL_SOURCES} src/app/linux/entry.cpp src/app/linux/platform_window.cpp
        src/app/linux/shutdown_signal.cpp src/app/linux/desktop_fonts.cpp
    LIBS maicord_compile_contract
        maicord_app_client maicord_ui_secure_text maicord_ui maicord_text_system maicord_text_catalog
        maicord_font_system maicord_font maicord_font_color maicord_theme maicord_unicode
        maicord_renderer maicord_atlas_gpu maicord_render_colr_paint maicord_paint_program maicord_render_core
        maicord_effect_host maicord_gpu_gles_video maicord_gpu_gles_shaders maicord_gpu_gles
        maicord_window_wayland maicord_audio_pulse maicord_aac_decoder maicord_video_decode
        maicord_desktop_system maicord_dbus maicord_model maicord_protocol maicord_voice_encoder
        maicord_notification_pcm maicord_media_core maicord_media_codec maicord_compression
        maicord_crypto maicord_net maicord_core
    INCLUDES src/app "${MAICORD_GENERATED_DIRECTORY}")
target_compile_options(MaiCord PRIVATE -Wno-c23-extensions)

# The shipped image has no symbol or string table, as /DEBUG:NONE leaves MaiCord.exe with none: the release job and every
# package take MaiCord as the build leaves it. MAICORD_KEEP_SYMBOLS (build_linux.sh --symbols) keeps them for nm, bloaty
# and profilers.
option(MAICORD_KEEP_SYMBOLS "Keep the symbol table in the Linux client (size and memory analysis)" OFF)
if(NOT MAICORD_KEEP_SYMBOLS)
    target_link_options(MaiCord PRIVATE LINKER:--strip-all)
endif()

# Runtime assets next to the executable, as on Windows: the bundled fonts the renderer reads from fonts/ and the licenses.
file(GLOB maicord_runtime_font_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*.otf"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL-*.txt")
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
add_dependencies(MaiCord maicord_runtime_fonts)

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

# The text the client shows names no Windows feature (tools/check_linux_ui_text.py reads the image; the settings catalog,
# which is stored deflated, is checked by maicord_linux_catalog_text).
add_test(NAME maicord_linux_client_text
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_linux_ui_text.py" "$<TARGET_FILE:MaiCord>")
set_tests_properties(maicord_linux_client_text PROPERTIES TIMEOUT 30)

# Cold code compiled for size, as the Windows client's (shared list: cmake/size_policy.cmake).
include(cmake/size_policy.cmake)
