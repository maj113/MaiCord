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
        maicord_desktop_system maicord_dbus maicord_atspi maicord_model maicord_protocol maicord_voice_encoder
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
include(cmake/runtime_assets.cmake)

# The text the client shows names no Windows feature (tools/check_linux_ui_text.py reads the image; the settings catalog,
# which is stored deflated, is checked by maicord_linux_catalog_text).
add_test(NAME maicord_linux_client_text
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_linux_ui_text.py" "$<TARGET_FILE:MaiCord>")
set_tests_properties(maicord_linux_client_text PROPERTIES TIMEOUT 30)

# `MaiCord --version` prints the build's number and nothing else (src/app/linux/entry.cpp).
add_test(NAME maicord_linux_client_version COMMAND MaiCord --version)
set_tests_properties(maicord_linux_client_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^MaiCord [0-9]+\\.[0-9]+\\.[0-9]+[^\n]*\n$" TIMEOUT 10)

# Cold code compiled for size, as the Windows client's (shared list: cmake/size_policy.cmake).
include(cmake/size_policy.cmake)
