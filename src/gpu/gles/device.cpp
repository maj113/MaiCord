#include "maicord/core/log.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/sys/linux/process.hpp"
#include "gpu/gles/internal.hpp"
#include "gpu/gles/video_planes_internal.hpp"

namespace maicord {
namespace gles {
namespace {

GpuDevice g_device;
GpuContext g_context;
void* g_requested_display; // gpu_device_set_native_display: copied into the device by gpu_device_create

constexpr gl::Enum kContextLost = 0x0507;
constexpr gl::Enum kOutOfMemory = 0x0505;

u32 append(char* destination, u32 capacity, u32 used, const char* text) noexcept {
    for (; text && *text && used + 1 < capacity; ++text)
        destination[used++] = *text;
    destination[used] = 0;
    return used;
}

bool open_display(GpuDevice& device) noexcept {
    egl::Int major = 0, minor = 0;
    const char* client_extensions = device.egl.eglQueryString(egl::kNoDisplay, egl::kExtensions);
    // A window system connection first when the caller has one: surfaces can only be made on that display.
    if (device.native_display && device.egl.eglGetPlatformDisplay) {
        device.display = device.egl.eglGetPlatformDisplay(egl::kPlatformWaylandKhr, device.native_display, nullptr);
        if (device.display && device.egl.eglInitialize(device.display, &major, &minor))
            return true;
        device.display = nullptr;
        device.native_display = nullptr;
    }
    if (device.egl.eglGetPlatformDisplay && string_has_word(client_extensions, "EGL_MESA_platform_surfaceless")) {
        device.display = device.egl.eglGetPlatformDisplay(egl::kPlatformSurfacelessMesa, nullptr, nullptr);
        if (device.display && device.egl.eglInitialize(device.display, &major, &minor))
            return true;
        device.display = nullptr;
    }
    device.display = device.egl.eglGetDisplay(nullptr);
    if (device.display && device.egl.eglInitialize(device.display, &major, &minor))
        return true;
    device.display = nullptr;
    return false;
}

// A context of the requested ES version; with `pbuffer` the config and a 1x1 surface for drivers that cannot make a
// context current without a surface.
bool make_current(GpuDevice& device, bool pbuffer) noexcept {
    const egl::Int attributes_surfaceless[] = {egl::kRenderableType, egl::kOpenGlEs3Bit, egl::kNone};
    const egl::Int attributes_pbuffer[] = {egl::kRenderableType, egl::kOpenGlEs3Bit, egl::kSurfaceType,
                                           egl::kPbufferBit,     egl::kRedSize,        8,
                                           egl::kGreenSize,      8,                    egl::kBlueSize,
                                           8,                    egl::kAlphaSize,      8,
                                           egl::kNone};
    // A window takes no alpha: the compositor then reads the surface's buffers as opaque (XRGB8888), which the window's
    // opaque region repeats. Any config of 8-bit colour does for the choice; the one without alpha is picked below.
    const egl::Int attributes_window[] = {egl::kRenderableType, egl::kOpenGlEs3Bit, egl::kSurfaceType,
                                          egl::kWindowBit,      egl::kRedSize,        8,
                                          egl::kGreenSize,      8,                    egl::kBlueSize,
                                          8,                    egl::kNone};
    // On a window system display the config can draw to windows (the surfaces made later share the context's
    // config); there is no pbuffer fallback there.
    const bool window = device.native_display != nullptr;
    if (window && pbuffer)
        return false;
    egl::Config config = nullptr;
    egl::Config candidates[32];
    egl::Int count = 0;
    if (!device.egl.eglBindAPI(egl::kOpenGlEsApi) ||
        !device.egl.eglChooseConfig(device.display,
                                    window ? attributes_window : (pbuffer ? attributes_pbuffer : attributes_surfaceless),
                                    candidates, window ? 32 : 1, &count) ||
        count < 1) {
        return false;
    }
    config = candidates[0];
    for (egl::Int i = 0; window && i < count; ++i) {
        egl::Int alpha = 1;
        if (device.egl.eglGetConfigAttrib(device.display, candidates[i], egl::kAlphaSize, &alpha) && alpha == 0) {
            config = candidates[i];
            break;
        }
    }
    const egl::Int context_attributes[] = {egl::kContextMajorVersion, 3, egl::kContextMinorVersion, 1, egl::kNone};
    device.context = device.egl.eglCreateContext(device.display, config, egl::kNoContext, context_attributes);
    if (!device.context)
        return false;
    egl::Surface surface = egl::kNoSurface;
    if (pbuffer) {
        const egl::Int surface_attributes[] = {egl::kWidth, 1, egl::kHeight, 1, egl::kNone};
        surface = device.egl.eglCreatePbufferSurface(device.display, config, surface_attributes);
        if (!surface) {
            device.egl.eglDestroyContext(device.display, device.context);
            device.context = nullptr;
            return false;
        }
    }
    if (!device.egl.eglMakeCurrent(device.display, surface, surface, device.context)) {
        if (surface)
            device.egl.eglDestroySurface(device.display, surface);
        device.egl.eglDestroyContext(device.display, device.context);
        device.context = nullptr;
        return false;
    }
    device.surface = surface;
    device.config = config;
    return true;
}

void describe(GpuDevice& device) noexcept {
    const gl::Api& api = device.gl;
    u32 used = 0;
    constexpr u32 capacity = sizeof(device.description);
    device.description[0] = 0;
    used = append(device.description, capacity, used, reinterpret_cast<const char*>(api.glGetString(gl::kVendor)));
    used = append(device.description, capacity, used, " | ");
    used = append(device.description, capacity, used, reinterpret_cast<const char*>(api.glGetString(gl::kRenderer)));
    used = append(device.description, capacity, used, " | ");
    append(device.description, capacity, used, reinterpret_cast<const char*>(api.glGetString(gl::kVersion)));
}

void probe_caps(GpuDevice& device, bool es32) noexcept {
    const gl::Api& api = device.gl;
    GpuCaps& caps = device.caps;
    gl::Int value = 0;
    api.glGetIntegerv(gl::kMaxTextureSize, &value);
    caps.max_texture_size = static_cast<u32>(value);
    api.glGetIntegerv(gl::kMaxUniformBlockSize, &value);
    caps.max_uniform_block_bytes = static_cast<u32>(value);
    api.glGetIntegerv(gl::kMaxComputeWorkGroupInvocations, &value);
    caps.max_compute_invocations = static_cast<u32>(value);
    // MAICORD_GLES_MAX_COMPUTE_INVOCATIONS=<n> lowers the group size limit the way a small device reports it (tests
    // only: the narrow shader variants and the decode gates).
    const char* invocations = environment_get("MAICORD_GLES_MAX_COMPUTE_INVOCATIONS");
    if (invocations && invocations[0] >= '0' && invocations[0] <= '9') {
        u32 limit = 0;
        for (const char* digit = invocations; *digit >= '0' && *digit <= '9'; ++digit)
            limit = limit * 10 + static_cast<u32>(*digit - '0');
        if (limit != 0 && limit < caps.max_compute_invocations)
            caps.max_compute_invocations = limit;
    }
    api.glGetIntegerv(gl::kMaxComputeSharedMemorySize, &value);
    caps.max_shared_bytes = static_cast<u32>(value);
    api.glGetIntegerv(gl::kMaxComputeShaderStorageBlocks, &value);
    caps.max_compute_storage_buffers = static_cast<u32>(value);
    caps.compute = true; // the context is ES 3.1 or newer
    caps.storage_rgba8 = true;
    caps.float_render_targets = es32 || has_extension(api, "GL_EXT_color_buffer_float");
    caps.half_render_targets =
        caps.float_render_targets || has_extension(api, "GL_EXT_color_buffer_half_float");
    caps.float_linear = has_extension(api, "GL_OES_texture_float_linear");
    // MAICORD_GLES_NO_DUAL_SOURCE=1 and MAICORD_GLES_NO_TIMER_QUERY=1 take the paths of drivers without
    // GL_EXT_blend_func_extended (the composite drawn in two halves) and GL_EXT_disjoint_timer_query (no GPU timing).
    const char* no_dual = environment_get("MAICORD_GLES_NO_DUAL_SOURCE");
    caps.dual_source_blend = has_extension(api, "GL_EXT_blend_func_extended") && !(no_dual && no_dual[0] == '1');
    const char* no_timer = environment_get("MAICORD_GLES_NO_TIMER_QUERY");
    caps.timer_query = false;
    device.gl.glGenQueriesEXT = nullptr;
    if (has_extension(api, "GL_EXT_disjoint_timer_query") && !(no_timer && no_timer[0] == '1')) {
        const auto load = [&](const char* name) noexcept { return device.egl.eglGetProcAddress(name); };
        device.gl.glGenQueriesEXT = reinterpret_cast<decltype(device.gl.glGenQueriesEXT)>(load("glGenQueriesEXT"));
        device.gl.glDeleteQueriesEXT = reinterpret_cast<decltype(device.gl.glDeleteQueriesEXT)>(load("glDeleteQueriesEXT"));
        device.gl.glQueryCounterEXT = reinterpret_cast<decltype(device.gl.glQueryCounterEXT)>(load("glQueryCounterEXT"));
        device.gl.glGetQueryObjectuivEXT =
            reinterpret_cast<decltype(device.gl.glGetQueryObjectuivEXT)>(load("glGetQueryObjectuivEXT"));
        device.gl.glGetQueryObjectui64vEXT =
            reinterpret_cast<decltype(device.gl.glGetQueryObjectui64vEXT)>(load("glGetQueryObjectui64vEXT"));
        caps.timer_query = device.gl.glGenQueriesEXT && device.gl.glDeleteQueriesEXT && device.gl.glQueryCounterEXT &&
                           device.gl.glGetQueryObjectuivEXT && device.gl.glGetQueryObjectui64vEXT;
    }
    caps.map_no_overwrite_on_srv = true; // glMapBufferRange with GL_MAP_UNSYNCHRONIZED_BIT
    const char* display_extensions = device.egl.eglQueryString(device.display, egl::kExtensions);
    caps.dmabuf_import = string_has_word(display_extensions, "EGL_EXT_image_dma_buf_import") &&
                         has_extension(api, "GL_OES_EGL_image");
    caps.surface_damage = string_has_word(display_extensions, "EGL_KHR_swap_buffers_with_damage") ||
                          string_has_word(display_extensions, "EGL_EXT_swap_buffers_with_damage");
    caps.buffer_age = string_has_word(display_extensions, "EGL_EXT_buffer_age");
    device.egl.eglSwapBuffersWithDamage = nullptr;
    if (caps.surface_damage) {
        const char* damage_names[] = {"eglSwapBuffersWithDamageKHR", "eglSwapBuffersWithDamageEXT"};
        for (const char* name : damage_names) {
            void* function = device.egl.eglGetProcAddress(name);
            if (function) {
                device.egl.eglSwapBuffersWithDamage = reinterpret_cast<decltype(device.egl.eglSwapBuffersWithDamage)>(function);
                break;
            }
        }
        caps.surface_damage = device.egl.eglSwapBuffersWithDamage != nullptr;
    }
    const char* copy_names[] = {"glCopyImageSubData", "glCopyImageSubDataOES", "glCopyImageSubDataEXT"};
    device.gl.glCopyImageSubData = nullptr;
    // MAICORD_GLES_NO_COPY_IMAGE=1 takes the framebuffer-copy path of drivers without copy-image (tests only).
    const char* no_copy = environment_get("MAICORD_GLES_NO_COPY_IMAGE");
    if ((!no_copy || no_copy[0] != '1') &&
        (es32 || has_extension(api, "GL_OES_copy_image") || has_extension(api, "GL_EXT_copy_image"))) {
        for (const char* name : copy_names) {
            void* function = device.egl.eglGetProcAddress(name);
            if (function) {
                device.gl.glCopyImageSubData = reinterpret_cast<decltype(device.gl.glCopyImageSubData)>(function);
                break;
            }
        }
    }
    caps.copy_image = device.gl.glCopyImageSubData != nullptr;
}

} // namespace

GpuDevice& device() noexcept {
    return g_device;
}

GpuContext& context() noexcept {
    return g_context;
}

GpuStatus check_error(const char* where) noexcept {
    GpuDevice& d = g_device;
    GpuStatus result = kGpuOk;
    for (u32 guard = 0; guard < 8; ++guard) {
        const gl::Enum error = d.gl.glGetError();
        if (error == gl::kNoError)
            break;
        log_write(LogLevel::error, "gpu", where);
        log_u64(LogLevel::error, "gpu", "gl_error", error);
        if (error == kContextLost) {
            d.lost = true;
            result = kGpuDeviceLost;
        } else if (result == kGpuOk) {
            result = error == kOutOfMemory ? kGpuOutOfMemory : kGpuFail;
        }
    }
    return result;
}

void flush_barriers() noexcept {
    GpuContext& c = g_context;
    if (c.pending_barriers) {
        g_device.gl.glMemoryBarrier(c.pending_barriers);
        c.pending_barriers = 0;
    }
}

bool texture_format(const GpuDevice& device, GpuFormat format, u8 bind, FormatInfo* info, bool* wide) noexcept {
    *wide = false;
    switch (format) {
    case GpuFormat::rgba8:
    case GpuFormat::bgra8:
        *info = {gl::kRgba8, gl::kRgba, gl::kUnsignedByte, 4};
        return true;
    case GpuFormat::r8:
        if (bind & gpu_bind_uav) {
            // 8-bit single-channel images are not storage-image formats in ES 3.1: the texture is RGBA8 and
            // the shader's coverage lands in every channel.
            *info = {gl::kRgba8, gl::kRgba, gl::kUnsignedByte, 4};
            *wide = true;
        } else {
            *info = {gl::kR8, gl::kRed, gl::kUnsignedByte, 1};
        }
        return true;
    case GpuFormat::rg8:
        *info = {gl::kRg8, gl::kRg, gl::kUnsignedByte, 2};
        return true;
    case GpuFormat::rgba16f:
        if ((bind & gpu_bind_rtv) && !device.caps.half_render_targets)
            return false;
        *info = {gl::kRgba16f, gl::kRgba, gl::kHalfFloat, 8};
        return true;
    case GpuFormat::rgba32f:
        if ((bind & gpu_bind_rtv) && !device.caps.float_render_targets)
            return false;
        *info = {gl::kRgba32f, gl::kRgba, gl::kFloat, 16};
        return true;
    case GpuFormat::r32_uint:
        return false;
    }
    return false;
}

u32 texture_framebuffer(GpuTexture* texture) noexcept {
    if (texture->framebuffer)
        return texture->framebuffer;
    GpuDevice& d = g_device;
    GpuContext& c = g_context;
    u32 name = 0;
    d.gl.glGenFramebuffers(1, &name);
    d.gl.glBindFramebuffer(gl::kFramebuffer, name);
    c.applied_draw_framebuffer = name;
    d.gl.glFramebufferTexture2D(gl::kFramebuffer, gl::kColorAttachment0, gl::kTexture2d, texture->name, 0);
    const gl::Enum status = d.gl.glCheckFramebufferStatus(gl::kFramebuffer);
    if (status != gl::kFramebufferComplete) {
        log_write(LogLevel::error, "gpu", "a texture cannot be a render target");
        log_u64(LogLevel::error, "gpu", "framebuffer_status", status);
        d.gl.glDeleteFramebuffers(1, &name);
        c.applied_draw_framebuffer = 0xFFFFFFFFu;
        return 0;
    }
    texture->framebuffer = name;
    return name;
}

} // namespace gles

void gpu_device_set_native_display(void* native_display) {
    gles::g_requested_display = native_display;
}

namespace gles {
namespace {

// The prestart worker (gpu_device_prestart): it makes the context on its thread, runs the continuation there and
// lets the context go; gpu_device_create then hands the device to the main thread, which takes the context over
// (claim_context) when the worker is done.
struct DevicePrestart {
    Thread* thread;
    Waitable ready;
    GpuDevicePrestartContinuation continuation;
    GpuStatus status;
    bool adopted;
};
DevicePrestart g_prestart;
bool g_prestarted_device; // the device in use came from the worker

GpuStatus create_device_here() noexcept {
    GpuDevice& d = gles::device();
    if (d.created)
        return kGpuFail;
    clear_bytes(&d, static_cast<u32>(sizeof(d)));
    clear_bytes(&gles::context(), static_cast<u32>(sizeof(GpuContext)));
    d.native_display = gles::g_requested_display;
    GpuStatus status = load_libraries(&d);
    if (!gpu_ok(status))
        return status;
    if (!open_display(d)) {
        log_write(LogLevel::error, "gpu", "EGL has no usable display");
        unload_libraries(&d);
        return kGpuUnsupported;
    }
    if (!make_current(d, false) && !make_current(d, true)) {
        log_write(LogLevel::error, "gpu", "no OpenGL ES 3.1 context could be created");
        d.egl.eglTerminate(d.display);
        unload_libraries(&d);
        return kGpuUnsupported;
    }
    gl::Int major = 0, minor = 0;
    d.gl.glGetIntegerv(gl::kMajorVersion, &major);
    d.gl.glGetIntegerv(gl::kMinorVersion, &minor);
    if (major < 3 || (major == 3 && minor < 1)) {
        log_write(LogLevel::error, "gpu", "the OpenGL ES context is older than 3.1");
        gpu_device_destroy(nullptr, nullptr);
        return kGpuUnsupported;
    }
    describe(d);
    probe_caps(d, major > 3 || minor >= 2);
    const char* checks = environment_get("MAICORD_GLES_CHECK");
    d.debug_checks = checks && checks[0] == '1';
    d.gl.glDisable(gl::kDither);
    d.gl.glDisable(gl::kCullFace);
    d.gl.glDisable(gl::kDepthTest);
    d.gl.glEnable(gl::kScissorTest);
    d.gl.glPixelStorei(gl::kUnpackAlignment, 1);
    d.gl.glPixelStorei(gl::kPackAlignment, 1);
    d.gl.glGenVertexArrays(1, &d.empty_vertex_array);
    d.buffers.reset();
    d.textures.reset();
    d.srvs.reset();
    d.uavs.reset();
    d.rtvs.reset();
    d.samplers.reset();
    d.blends.reset();
    d.rasters.reset();
    d.vertex_shaders.reset();
    d.pixel_shaders.reset();
    d.compute_shaders.reset();
    d.layouts.reset();
    d.queries.reset();
    d.fences.reset();
    GpuContext& c = gles::context();
    c.applied_draw_framebuffer = 0xFFFFFFFFu;
    c.applied_program = 0xFFFFFFFFu;
    c.applied_vertex_array = 0xFFFFFFFFu;
    c.applied_vertex_buffer = 0xFFFFFFFFu;
    status = check_error("device initialization");
    if (!gpu_ok(status)) {
        gpu_device_destroy(nullptr, nullptr);
        return status;
    }
    d.created = true;
    log_write(LogLevel::info, "gpu", d.description);
    return kGpuOk;
}

u32 prestart_worker(void*) noexcept {
    DevicePrestart& prestart = g_prestart;
    prestart.status = create_device_here();
    const bool made = gpu_ok(prestart.status);
    event_set(prestart.ready);
    if (made && prestart.continuation)
        prestart.continuation(&gles::device(), &gles::context());
    if (made) {
        // The context is current on this thread until it is let go; the owner takes it over in claim_context.
        GpuDevice& d = gles::device();
        d.egl.eglMakeCurrent(d.display, egl::kNoSurface, egl::kNoSurface, egl::kNoContext);
    }
    return 0;
}

// Waits for the worker to return and forgets it.
void reap_prestart() noexcept {
    if (g_prestart.thread) {
        thread_wait(g_prestart.thread, 0xFFFFFFFFu);
        thread_close(g_prestart.thread);
    }
    waitable_close(g_prestart.ready);
    g_prestart = {};
}

} // namespace

void claim_context() noexcept {
    if (!g_prestart.thread)
        return;
    GpuDevice& d = gles::device();
    thread_wait(g_prestart.thread, 0xFFFFFFFFu);
    thread_close(g_prestart.thread);
    g_prestart.thread = nullptr;
    if (d.created)
        d.egl.eglMakeCurrent(d.display, d.surface, d.surface, d.context);
}

} // namespace gles

GpuStatus gpu_device_create(GpuDevice** out_device, GpuContext** out_context) {
    using namespace gles;
    if (g_prestart.ready) {
        wait_one(g_prestart.ready, 0xFFFFFFFFu);
        if (gpu_ok(g_prestart.status)) {
            // The worker made the device; its continuation may still be using the context.
            g_prestart.adopted = true;
            g_prestarted_device = true;
            *out_device = &gles::device();
            *out_context = &gles::context();
            return kGpuOk;
        }
        reap_prestart(); // it failed: try again here, driver failures can be transient
    }
    const GpuStatus status = create_device_here();
    if (!gpu_ok(status))
        return status;
    *out_device = &gles::device();
    *out_context = &gles::context();
    return kGpuOk;
}

void gpu_device_prestart(GpuDevicePrestartContinuation continuation) {
    using namespace gles;
    if (g_prestart.ready)
        return;
    g_prestart.ready = event_create(true, false);
    if (!g_prestart.ready)
        return;
    g_prestart.continuation = continuation;
    g_prestart.thread = thread_start(prestart_worker, nullptr, 1024u * 1024u, nullptr);
    if (!g_prestart.thread) {
        waitable_close(g_prestart.ready);
        g_prestart = {};
    }
}

void gpu_device_prestart_wait() {
    gles::claim_context();
}

void gpu_device_prestart_finish() {
    using namespace gles;
    if (!g_prestart.ready)
        return;
    claim_context();
    const bool adopted = g_prestart.adopted;
    waitable_close(g_prestart.ready);
    g_prestart = {};
    if (!adopted && gles::device().created)
        gpu_device_destroy(nullptr, nullptr); // nobody took the device the worker made
}

bool gpu_device_prestarted(const GpuDevice* device) {
    return device && device == &gles::device() && gles::g_prestarted_device;
}

void gpu_device_destroy(GpuDevice** out_device, GpuContext** out_context) {
    GpuDevice& d = gles::device();
    if (!d.display && !d.egl_library)
        return;
    if (d.display) {
        gles::video_planes_shutdown(); // the EGLImages of decoded pictures go before the display
        d.egl.eglMakeCurrent(d.display, egl::kNoSurface, egl::kNoSurface, egl::kNoContext);
        // The GL objects of the context go with it.
        if (d.context)
            d.egl.eglDestroyContext(d.display, d.context);
        if (d.surface)
            d.egl.eglDestroySurface(d.display, d.surface);
        d.egl.eglTerminate(d.display);
    }
    gles::unload_libraries(&d);
    clear_bytes(&d, static_cast<u32>(sizeof(d)));
    gles::g_prestarted_device = false;
    if (out_device)
        *out_device = nullptr;
    if (out_context)
        *out_context = nullptr;
}

const GpuCaps& gpu_caps(const GpuDevice* device) {
    return device->caps;
}

const char* gpu_device_description(const GpuDevice* device) {
    return device->description;
}

bool gpu_device_lost(GpuDevice* device) {
    return device->lost;
}

u64 gpu_adapter_key(const GpuDevice* device) {
    // The vendor, renderer and version strings name the GPU and its driver stack (FNV-1a, 64 bits).
    u64 hash = 0xcbf29ce484222325ull;
    for (const char* text = device->description; *text; ++text)
        hash = (hash ^ static_cast<u8>(*text)) * 0x100000001b3ull;
    return hash;
}

GpuContext1* gpu_context_1(GpuContext* context) {
    return context;
}

GpuDevice* gpu_context_device(GpuContext*) {
    return &gles::device();
}

} // namespace maicord
