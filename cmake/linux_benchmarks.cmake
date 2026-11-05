# Opt-in benchmarks of the Linux port (tools/build_linux.sh --benchmarks). The chat timeline benchmark is the Windows
# one (cmake/benchmarks.cmake): the same source, over the Wayland window and the GLES backend. tools/benchmark_chat_list.py
# runs it.
option(MAICORD_BUILD_BENCHMARKS "Build opt-in benchmark executables" OFF)
if(NOT MAICORD_BUILD_BENCHMARKS)
    return()
endif()

# The units of the message runtime (maicord_message_runtime on Windows) that the timeline needs; on Linux they are
# part of the client's application library, which also holds the shell's dependencies, so the program takes them itself.
# --wrap makes the image's malloc, calloc and realloc calls pass the counters of benchmarks/chat_list_benchmark.cpp.
maicord_program(MaiCordChatListBenchmark ENTRY maicord_chat_list_benchmark_entry
    SOURCES benchmarks/chat_list_benchmark.cpp src/app/chat/messages/history_sync.cpp
        src/app/chat/messages/timeline.cpp src/app/chat/messages/view_runtime.cpp
        src/model/chat/message_reconcile.cpp
    LIBS maicord_compile_contract
        maicord_ui maicord_text_system maicord_text_catalog maicord_font_system maicord_font maicord_font_color
        maicord_theme maicord_unicode maicord_renderer maicord_atlas_gpu maicord_render_colr_paint maicord_paint_program
        maicord_render_core maicord_effect_host maicord_gpu_gles_video maicord_gpu_gles_shaders maicord_gpu_gles
        maicord_window_wayland maicord_desktop_system maicord_dbus maicord_model maicord_protocol maicord_media_core
        maicord_media_codec maicord_compression maicord_core
    LINK_OPTIONS LINKER:--wrap=malloc LINKER:--wrap=calloc LINKER:--wrap=realloc
    INCLUDES src/app "${MAICORD_GENERATED_DIRECTORY}")
target_compile_options(MaiCordChatListBenchmark PRIVATE -Wno-c23-extensions)
