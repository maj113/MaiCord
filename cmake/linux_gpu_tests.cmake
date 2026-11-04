# The GPU tests of the Linux build. They need an EGL driver: Mesa's llvmpipe is the CI default, and a developer GPU
# is selected the way the driver stack does it (on WSL: GALLIUM_DRIVER=d3d12 tools/build_linux.sh --test ...).

# The device made by the prestart worker, views that keep their texture, retains, timers and fences on a real driver.
maicord_test(MaiCordGpuSyncTests ENTRY maicord_gpu_sync_entry
    SOURCES tests/gpu/sync.cpp
    LIBS maicord_gpu_gles maicord_core
    TIMEOUT 20 TEST maicord_gpu_sync)
maicord_add_tests(MaiCordGpuSyncTests TIMEOUT 20 ENV MAICORD_TEST_PRESTART=0 TEST maicord_gpu_sync_no_prestart)
# A driver without timer queries: the create fails the way it does on a device without them.
maicord_add_tests(MaiCordGpuSyncTests TIMEOUT 20 ENV MAICORD_GLES_NO_TIMER_QUERY=1 TEST maicord_gpu_sync_no_timers)

maicord_test(MaiCordGpuClearReadbackTests ENTRY maicord_gpu_clear_readback_entry
    SOURCES tests/gpu/clear_readback.cpp
    LIBS maicord_gpu_gles maicord_core
    TIMEOUT 10 TEST maicord_gpu_clear_readback)
maicord_add_tests(MaiCordGpuClearReadbackTests TIMEOUT 10
    ENV MAICORD_GLES_NO_COPY_IMAGE=1
    TEST maicord_gpu_clear_readback_copy_fallback)

# What draws or dispatches needs the shader pack too (the pinned toolchain builds it on first use).
maicord_test(MaiCordGpuShaderPackTests ENTRY maicord_gpu_shader_pack_entry
    SOURCES tests/gpu/shader_pack.cpp
    LIBS maicord_gpu_gles_shaders maicord_gpu_gles maicord_compression maicord_core INCLUDES src
    TIMEOUT 20 TEST maicord_gpu_shader_pack)
# A device that offers the 128 invocations per group the specification guarantees: colr_layers links as its narrow variant.
maicord_add_tests(MaiCordGpuShaderPackTests TIMEOUT 20
    ENV MAICORD_GLES_MAX_COMPUTE_INVOCATIONS=128
    TEST maicord_gpu_shader_pack_small_groups)
# A driver without GL_EXT_blend_func_extended: the composite pixel shader is made as its weight and colour halves.
maicord_add_tests(MaiCordGpuShaderPackTests TIMEOUT 20
    ENV MAICORD_GLES_NO_DUAL_SOURCE=1
    TEST maicord_gpu_shader_pack_no_dual_source)

# The ui quads scene against the PNG that tests/gpu/ui_quads_windows.cpp captured from Direct3D 11.
maicord_test(MaiCordGpuUiQuadsTests ENTRY maicord_gpu_ui_quads_entry
    SOURCES tests/gpu/ui_quads.cpp
    LIBS maicord_gpu_gles_shaders maicord_gpu_gles maicord_compression maicord_core
    TIMEOUT 20 TEST maicord_gpu_ui_quads)

# The glyph scene (compute rasterization into the atlas, then the glyph shaders' draw) against the PNGs that
# tests/gpu/glyphs_windows.cpp captured from Direct3D 11.
maicord_test(MaiCordGpuGlyphsTests ENTRY maicord_gpu_glyphs_entry
    SOURCES tests/gpu/glyphs.cpp
    LIBS maicord_gpu_gles_shaders maicord_gpu_gles maicord_compression maicord_core
    TIMEOUT 20 TEST maicord_gpu_glyphs)

# The media scene (PNG, VP8 and JPEG compute into an RGBA8 atlas, then the media shaders' draw) against the PNGs that
# tests/gpu/media_windows.cpp captured from Direct3D 11.
maicord_test(MaiCordGpuMediaTests ENTRY maicord_gpu_media_entry
    SOURCES tests/gpu/media.cpp
    LIBS maicord_gpu_gles_shaders maicord_gpu_gles maicord_compression maicord_core
    TIMEOUT 20 TEST maicord_gpu_media)

# Every sample effect and Silk Waves, as interface 4 packages, through the real pass executor against the pictures
# Direct3D 11 made (the same source runs on Windows and captures them). The cost probe is off so a slow software
# renderer can neither refuse an effect nor lower its resolution; the second run draws the composite without
# dual-source blending.
maicord_test(MaiCordGpuEffectsTests ENTRY maicord_gpu_effects_entry
    SOURCES tests/gpu/effects.cpp
    LIBS maicord_effect_host maicord_gpu_gles_shaders maicord_gpu_gles maicord_compression maicord_media_codec
        maicord_core
    INCLUDES src
    TIMEOUT 120 ENV MAICORD_GLES_NO_TIMER_QUERY=1 TEST maicord_gpu_effects)
maicord_add_tests(MaiCordGpuEffectsTests TIMEOUT 120
    ENV MAICORD_GLES_NO_TIMER_QUERY=1 MAICORD_GLES_NO_DUAL_SOURCE=1
    TEST maicord_gpu_effects_no_dual_source)

# A window on a compositor showing the same scene through the EGL window surface, the damage blit and the frame
# callbacks. Opt-in like the Wayland window tests (a window opens): MAICORD_WAYLAND_INTEGRATION=1 at configure time;
# tools/test_gpu_surface.sh runs it on headless sway with a screenshot check. Status 77: no compositor.
maicord_test(MaiCordGpuSurfaceWaylandTests ENTRY maicord_gpu_surface_wayland_entry
    SOURCES tests/gpu/surface_wayland.cpp
    LIBS maicord_gpu_gles_shaders maicord_gpu_gles maicord_window_wayland maicord_compression maicord_core
    TIMEOUT 20 LABELS integration TEST maicord_gpu_surface_wayland)
set_tests_properties(maicord_gpu_surface_wayland PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK wayland_compositor)
if(NOT "$ENV{MAICORD_WAYLAND_INTEGRATION}")
    set_tests_properties(maicord_gpu_surface_wayland PROPERTIES DISABLED TRUE)
endif()
