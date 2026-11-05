# The OpenGL ES GPU backend of the Linux build (src/gpu/gles, behind include/maicord/gpu/api.hpp). It never enters the
# Windows build: that one compiles src/render/d3d11 behind the same handles.
#
# maicord_gpu_gles          EGL device (libEGL.so.1 and libGLESv2.so.2 by dlopen), caps probe, resources, state tracker
# maicord_gpu_gles_shaders  the GLSL shader pack (tools/build_gles_shader_pack.py) and the creation of the built-ins
#
# Tests that only clear, copy and read back link the first; anything that draws or dispatches needs both. The pack
# needs the pinned toolchain (tools/linux_shader_toolchain.json: DXC and SPIRV-Cross, fetched and built into
# $MAICORD_TOOLCHAIN_CACHE, default ~/.cache/maicord-toolchain, by tools/fetch_linux_shader_toolchain.py), so the
# second library is not part of the default build: the tests that link it build it.
#
# A build offline from distribution packages sets MAICORD_DXC (the dxc executable) and MAICORD_SPIRV_CROSS_PREFIX (an
# installed SPIRV-Cross with its static libraries) instead: nothing is fetched, only mc_shadertool is compiled against
# that SPIRV-Cross, with the C++ compiler named by $CXX (a hosted one, not this build's freestanding clang). They are
# the script's MAICORD_DXC and MAICORD_SPIRV_CROSS and are set together or not at all.
set(MAICORD_DXC "" CACHE FILEPATH "dxc executable to translate the shaders with, instead of the pinned download")
set(MAICORD_SPIRV_CROSS_PREFIX "" CACHE PATH "Prefix of an installed SPIRV-Cross, instead of the pinned build")
set(maicord_shader_tool_environment "")
if(MAICORD_DXC OR MAICORD_SPIRV_CROSS_PREFIX)
    if(NOT MAICORD_DXC OR NOT MAICORD_SPIRV_CROSS_PREFIX)
        message(FATAL_ERROR "Set MAICORD_DXC and MAICORD_SPIRV_CROSS_PREFIX together, or neither.")
    endif()
    set(maicord_shader_tool_environment ${CMAKE_COMMAND} -E env "MAICORD_DXC=${MAICORD_DXC}"
        "MAICORD_SPIRV_CROSS=${MAICORD_SPIRV_CROSS_PREFIX}")
endif()

maicord_library(maicord_gpu_gles
    src/gpu/gles/loader.cpp src/gpu/gles/device.cpp src/gpu/gles/resources.cpp src/gpu/gles/draw.cpp
    src/gpu/gles/surface.cpp src/gpu/gles/sync.cpp src/gpu/gles/video_planes.cpp
    LIBS maicord_core INCLUDES src)

set(MAICORD_GLES_SHADER_PACK "${MAICORD_GENERATED_DIRECTORY}/maicord_shader_pack_gles.inc")
file(GLOB maicord_hlsl_sources CONFIGURE_DEPENDS src/render/shaders/*.hlsl)
add_custom_command(
    OUTPUT "${MAICORD_GLES_SHADER_PACK}"
    BYPRODUCTS "${MAICORD_GENERATED_DIRECTORY}/maicord_shader_pack_gles.bin"
    COMMAND ${maicord_shader_tool_environment} "${Python3_EXECUTABLE}" -B tools/build_gles_shader_pack.py "${MAICORD_GLES_SHADER_PACK}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS tools/build_gles_shader_pack.py tools/glsl_translate.py tools/fetch_linux_shader_toolchain.py
        tools/linux_shader_toolchain.json
        tools/shadertool/mc_shadertool.cpp tools/optimal_deflate.py tools/deflate_cache.txt
        tools/font_reference/embed_fixture.py cmake/generated_sources.cmake ${maicord_hlsl_sources}
    VERBATIM)
maicord_library(maicord_gpu_gles_shaders src/gpu/gles/shader_pack.cpp
    LIBS maicord_gpu_gles maicord_compression maicord_core INCLUDES src "${MAICORD_GENERATED_DIRECTORY}")
set_source_files_properties(src/gpu/gles/shader_pack.cpp PROPERTIES OBJECT_DEPENDS "${MAICORD_GLES_SHADER_PACK}")
set_target_properties(maicord_gpu_gles_shaders PROPERTIES EXCLUDE_FROM_ALL TRUE)
# The pack is #embed'ed (a C23 extension to clang's C++).
target_compile_options(maicord_gpu_gles_shaders PRIVATE -Wno-c23-extensions)

# The effect host: the pass-graph executor and inputs of .mcfx background effects (the code Windows runs, over the GPU
# API) and the package parser. The default theme the executor takes its palette from is maicord_theme's
# (cmake/linux_render.cmake).
maicord_library(maicord_effect_host
    src/render/d3d11/effect_renderer.cpp src/render/d3d11/effect_inputs.cpp src/app/packages/mcfx.cpp
    src/app/packages/link.cpp
    LIBS maicord_gpu_gles maicord_media_codec maicord_theme maicord_core INCLUDES src)
target_compile_options(maicord_effect_host PRIVATE -Wno-c23-extensions)
