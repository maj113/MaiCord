#include "maicord/core/log.hpp"
#include "maicord/core/shared_library.hpp"
#include "gpu/gles/internal.hpp"

namespace maicord::gles {
namespace {

SharedLibrary g_egl_library;
SharedLibrary g_gles_library;

// The function tables as data: every name after the last, NUL terminated, and where its pointer sits in the Api. The
// loader is one loop over them, not a lookup, a check and a log line of code per function.
#define MAICORD_EGL_NAME(result, name, arguments) #name "\0"
#define MAICORD_EGL_SLOT(result, name, arguments) static_cast<u16>(__builtin_offsetof(egl::Api, name)),
#define MAICORD_GLES_NAME(result, name, arguments) #name "\0"
#define MAICORD_GLES_SLOT(result, name, arguments) static_cast<u16>(__builtin_offsetof(gl::Api, name)),
constexpr char kEglNames[] = MAICORD_EGL_FUNCTIONS(MAICORD_EGL_NAME);
constexpr u16 kEglSlots[] = {MAICORD_EGL_FUNCTIONS(MAICORD_EGL_SLOT)};
constexpr char kGlesNames[] = MAICORD_GLES_FUNCTIONS(MAICORD_GLES_NAME);
constexpr u16 kGlesSlots[] = {MAICORD_GLES_FUNCTIONS(MAICORD_GLES_SLOT)};
#undef MAICORD_EGL_NAME
#undef MAICORD_EGL_SLOT
#undef MAICORD_GLES_NAME
#undef MAICORD_GLES_SLOT

void fail(const char* message) noexcept {
    log_write(LogLevel::error, "gpu", message);
    const char* detail = shared_library_error();
    if (detail)
        log_write(LogLevel::error, "gpu", detail);
}

// Stores each named function of `library` in its slot of `api` and logs "<lacks><name>" for every one it does not
// have; false when any is missing.
__declspec(noinline) bool resolve(const SharedLibrary& library, void* api, const char* names, const u16* slots,
                                  u32 count, const char* lacks) noexcept {
    char message[64];
    const u32 prefix = c_string_length(lacks);
    copy_bytes(message, lacks, prefix);
    bool complete = true;
    for (u32 index = 0; index < count; ++index) {
        const u32 length = c_string_length(names) + 1;
        void* function = shared_library_symbol(library, names);
        copy_bytes(static_cast<u8*>(api) + slots[index], &function, sizeof(function));
        if (!function) {
            copy_bytes(message + prefix, names, length);
            log_write(LogLevel::error, "gpu", message);
            complete = false;
        }
        names += length;
    }
    return complete;
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

    const bool egl_complete = resolve(g_egl_library, &device->egl, kEglNames, kEglSlots,
                                      sizeof(kEglSlots) / sizeof(kEglSlots[0]), "libEGL.so.1 lacks ");
    const bool complete = resolve(g_gles_library, &device->gl, kGlesNames, kGlesSlots,
                                  sizeof(kGlesSlots) / sizeof(kGlesSlots[0]), "libGLESv2.so.2 lacks ") &&
                          egl_complete;
    if (!complete) {
        unload_libraries(device);
        return kGpuUnsupported;
    }
    return kGpuOk;
}

void unload_libraries(GpuDevice* device) noexcept {
    shared_library_close(&g_gles_library);
    shared_library_close(&g_egl_library);
    device->egl_library = nullptr;
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
