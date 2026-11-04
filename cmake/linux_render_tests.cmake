# The renderer tests of the Linux build: the shared renderer (cmake/linux_render.cmake) over the OpenGL ES backend and an
# offscreen GpuSurface. They need an EGL driver like the GPU tests (llvmpipe anywhere, GALLIUM_DRIVER=d3d12 on WSL).
set(maicord_render_test_libs
    maicord_renderer maicord_atlas_gpu maicord_paint_program maicord_render_colr_paint maicord_effect_host
    maicord_ui maicord_text_system maicord_text_catalog maicord_font_system maicord_font_color maicord_font
    maicord_unicode maicord_model maicord_app_update maicord_media_core maicord_media_codec maicord_compression
    maicord_render_core maicord_theme maicord_gpu_gles_video maicord_gpu_gles_shaders maicord_gpu_gles maicord_core)

# The bootstrap title at three DPIs through the standalone draw path: the glyph pipeline (compute rasterization into the
# atlas, quads) and the ui shaders end to end.
maicord_test(MaiCordRenderScenesTests ENTRY maicord_render_scenes_entry
    SOURCES tests/render/scenes.cpp src/app/chat/messages/timeline.cpp
    LIBS ${maicord_render_test_libs}
    INCLUDES src "${MAICORD_GENERATED_DIRECTORY}"
    TIMEOUT 20 TEST maicord_render_scenes)
