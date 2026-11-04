#include "maicord/core/log.hpp"
#include "maicord/core/shared_library.hpp"
#include "gpu/gles/internal.hpp"

namespace maicord::gles {
namespace {

SharedLibrary g_egl_library;
SharedLibrary g_gles_library;

void fail(const char* message) noexcept {
    log_write(LogLevel::error, "gpu", message);
    const char* detail = shared_library_error();
    if (detail)
        log_write(LogLevel::error, "gpu", detail);
}

} // namespace

GpuStatus load_libraries(GpuDevice* device) noexcept {
    if (!shared_library_open(&g_egl_library, "libEGL.so.1")) {
        fail("libEGL.so.1 is not available");
        return kGpuUnsupported;
    }
    if (!shared_library_open(&g_gles_library, "libGLESv2.so.2")) {
        fail("libGLESv2.so.2 is not available");
        shared_library_close(&g_egl_library);
        return kGpuUnsupported;
    }
    device->egl_library = g_egl_library.handle;
    device->gles_library = g_gles_library.handle;

    bool complete = true;
#define MAICORD_EGL_LOAD(result, name, arguments)                                                                      \
    device->egl.name = reinterpret_cast<result(*) arguments>(shared_library_symbol(g_egl_library, #name));             \
    if (!device->egl.name) {                                                                                           \
        log_write(LogLevel::error, "gpu", "libEGL.so.1 lacks " #name);                                                 \
        complete = false;                                                                                              \
    }
    {
        using namespace egl; // the table's argument types are spelled unqualified
        MAICORD_EGL_FUNCTIONS(MAICORD_EGL_LOAD)
    }
#undef MAICORD_EGL_LOAD
#define MAICORD_GLES_LOAD(result, name, arguments)                                                                     \
    device->gl.name = reinterpret_cast<result(*) arguments>(shared_library_symbol(g_gles_library, #name));             \
    if (!device->gl.name) {                                                                                            \
        log_write(LogLevel::error, "gpu", "libGLESv2.so.2 lacks " #name);                                              \
        complete = false;                                                                                              \
    }
    {
        using namespace gl;
        MAICORD_GLES_FUNCTIONS(MAICORD_GLES_LOAD)
    }
#undef MAICORD_GLES_LOAD
    if (!complete) {
        unload_libraries(device);
        return kGpuUnsupported;
    }
    // EGL 1.5 exports eglGetPlatformDisplay; older libraries only offer the extension through eglGetProcAddress.
    device->egl.eglGetPlatformDisplay = reinterpret_cast<egl::Display (*)(egl::Enum, void*, const void*)>(
        shared_library_symbol(g_egl_library, "eglGetPlatformDisplay"));
    if (!device->egl.eglGetPlatformDisplay) {
        device->egl.eglGetPlatformDisplay = reinterpret_cast<egl::Display (*)(egl::Enum, void*, const void*)>(
            device->egl.eglGetProcAddress("eglGetPlatformDisplayEXT"));
    }
    return kGpuOk;
}

void unload_libraries(GpuDevice* device) noexcept {
    shared_library_close(&g_gles_library);
    shared_library_close(&g_egl_library);
    device->egl_library = nullptr;
    device->gles_library = nullptr;
}

bool string_has_word(const char* list, const char* word) noexcept {
    if (!list)
        return false;
    const u32 length = c_string_length(word);
    while (*list) {
        while (*list == ' ')
            ++list;
        const char* end = list;
        while (*end && *end != ' ')
            ++end;
        if (static_cast<u32>(end - list) == length && bytes_equal(list, word, length))
            return true;
        list = end;
    }
    return false;
}

bool has_extension(const gl::Api& api, const char* name) noexcept {
    gl::Int count = 0;
    api.glGetIntegerv(gl::kNumExtensions, &count);
    const u32 length = c_string_length(name);
    for (gl::Int index = 0; index < count; ++index) {
        const char* extension = reinterpret_cast<const char*>(api.glGetStringi(gl::kExtensions, static_cast<u32>(index)));
        if (extension && c_string_length(extension) == length && bytes_equal(extension, name, length))
            return true;
    }
    return false;
}

} // namespace maicord::gles
