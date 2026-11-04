# The GPU tests of the Linux build. They need an EGL driver: Mesa's llvmpipe is the CI default, and a developer GPU
# is selected the way the driver stack does it (on WSL: GALLIUM_DRIVER=d3d12 tools/build_linux.sh --test ...).

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

# The ui quads scene against the PNG that tests/gpu/ui_quads_windows.cpp captured from Direct3D 11.
maicord_test(MaiCordGpuUiQuadsTests ENTRY maicord_gpu_ui_quads_entry
    SOURCES tests/gpu/ui_quads.cpp
    LIBS maicord_gpu_gles_shaders maicord_gpu_gles maicord_compression maicord_core
    TIMEOUT 20 TEST maicord_gpu_ui_quads)
