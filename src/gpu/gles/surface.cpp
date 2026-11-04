#include "maicord/core/log.hpp"
#include "maicord/core/shared_library.hpp"
#include "gpu/gles/internal.hpp"

namespace maicord {
namespace {

using namespace gles;

GlesSurface g_surface;

struct WaylandEglApi {
    void* (*create)(void* surface, int width, int height);
    void (*destroy)(void* window);
    void (*resize)(void* window, int width, int height, int dx, int dy);
};
SharedLibrary g_wayland_egl;
WaylandEglApi g_wayland_egl_api;

bool load_wayland_egl() noexcept {
    if (g_wayland_egl_api.create)
        return true;
    if (!shared_library_open(&g_wayland_egl, "libwayland-egl.so.1")) {
        log_write(LogLevel::error, "gpu", "libwayland-egl.so.1 is not available");
        return false;
    }
    g_wayland_egl_api.create = reinterpret_cast<decltype(g_wayland_egl_api.create)>(
        shared_library_symbol(g_wayland_egl, "wl_egl_window_create"));
    g_wayland_egl_api.destroy = reinterpret_cast<decltype(g_wayland_egl_api.destroy)>(
        shared_library_symbol(g_wayland_egl, "wl_egl_window_destroy"));
    g_wayland_egl_api.resize = reinterpret_cast<decltype(g_wayland_egl_api.resize)>(
        shared_library_symbol(g_wayland_egl, "wl_egl_window_resize"));
    if (!g_wayland_egl_api.create || !g_wayland_egl_api.destroy || !g_wayland_egl_api.resize) {
        g_wayland_egl_api = {};
        shared_library_close(&g_wayland_egl);
        log_write(LogLevel::error, "gpu", "libwayland-egl.so.1 lacks wl_egl_window_*");
        return false;
    }
    return true;
}

// The bounding box of the rectangles, clamped to the frame.
GlesDamage damage_of(const GpuRect* dirty, u32 count, u32 width, u32 height) noexcept {
    GlesDamage damage{};
    if (!dirty || count == 0) {
        damage.full = true;
        return damage;
    }
    bool any = false;
    for (u32 index = 0; index < count; ++index) {
        GpuRect rectangle = dirty[index];
        rectangle.left = rectangle.left < 0 ? 0 : rectangle.left;
        rectangle.top = rectangle.top < 0 ? 0 : rectangle.top;
        rectangle.right = rectangle.right > static_cast<i32>(width) ? static_cast<i32>(width) : rectangle.right;
        rectangle.bottom = rectangle.bottom > static_cast<i32>(height) ? static_cast<i32>(height) : rectangle.bottom;
        if (rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top)
            continue;
        if (!any) {
            damage.box = rectangle;
            any = true;
        } else {
            damage.box.left = rectangle.left < damage.box.left ? rectangle.left : damage.box.left;
            damage.box.top = rectangle.top < damage.box.top ? rectangle.top : damage.box.top;
            damage.box.right = rectangle.right > damage.box.right ? rectangle.right : damage.box.right;
            damage.box.bottom = rectangle.bottom > damage.box.bottom ? rectangle.bottom : damage.box.bottom;
        }
    }
    return damage; // nothing changed: an empty box, an empty blit and a swap that damages nothing
}

void merge(GpuRect* into, const GpuRect& other) noexcept {
    if (into->right <= into->left) {
        *into = other;
        return;
    }
    into->left = other.left < into->left ? other.left : into->left;
    into->top = other.top < into->top ? other.top : into->top;
    into->right = other.right > into->right ? other.right : into->right;
    into->bottom = other.bottom > into->bottom ? other.bottom : into->bottom;
}

GpuStatus make_frame(GlesSurface* surface, u32 width, u32 height) noexcept {
    GpuDevice& device = gles::device();
    GpuTexture* frame = nullptr;
    GpuRtv* target = nullptr;
    const GpuTextureDesc description{width, height, GpuFormat::rgba8, GpuUsage::default_,
                                     static_cast<u8>(gpu_bind_srv | gpu_bind_rtv)};
    GpuStatus status = gpu_texture_create(&device, description, nullptr, &frame);
    if (gpu_ok(status))
        status = gpu_rtv_create(&device, frame, &target);
    if (!gpu_ok(status)) {
        gpu_release(frame);
        return status;
    }
    gpu_release(surface->own_target);
    gpu_release(surface->frame);
    surface->frame = frame;
    surface->own_target = target;
    surface->target = target;
    surface->width = width;
    surface->height = height;
    surface->history_count = 0;
    surface->needs_full = true;
    return kGpuOk;
}

// The window half of a surface: made not current, then the EGL surface and the wl_egl_window go. Offscreen has neither.
void release_window(GpuDevice* device, GlesSurface* s) noexcept {
    if (s->surface) {
        device->egl.eglMakeCurrent(device->display, egl::kNoSurface, egl::kNoSurface, device->context);
        device->egl.eglDestroySurface(device->display, s->surface);
    }
    if (s->window)
        g_wayland_egl_api.destroy(s->window);
}

} // namespace

GpuStatus gpu_surface_create(GpuDevice* device, const GpuSurfaceDesc& desc, GpuSurface* out) {
    GlesSurface& s = g_surface;
    if (out->impl || s.created || desc.width == 0 || desc.height == 0)
        return kGpuInvalidArgument;
    // A null window is an offscreen surface: the frame target alone, for tests and tools that read the pictures back.
    const bool windowed = desc.window != nullptr;
    if (windowed && !device->native_display) {
        log_write(LogLevel::error, "gpu", "a surface needs a device made with gpu_device_set_native_display");
        return kGpuUnsupported;
    }
    if (windowed && !load_wayland_egl())
        return kGpuUnsupported;
    clear_bytes(&s, static_cast<u32>(sizeof(s)));
    if (windowed) {
        s.window = g_wayland_egl_api.create(desc.window, static_cast<int>(desc.width), static_cast<int>(desc.height));
        if (!s.window) {
            log_write(LogLevel::error, "gpu", "wl_egl_window_create failed");
            return kGpuFail;
        }
        s.surface = device->egl.eglCreateWindowSurface(device->display, device->config, s.window, nullptr);
        if (!s.surface) {
            log_write(LogLevel::error, "gpu", "eglCreateWindowSurface failed");
            log_u64(LogLevel::error, "gpu", "egl_error", static_cast<u32>(device->egl.eglGetError()));
            release_window(device, &s);
            s = {};
            return kGpuFail;
        }
    }
    gles::claim_context(); // the prestart worker must be done with the context first
    if (windowed && !device->egl.eglMakeCurrent(device->display, s.surface, s.surface, device->context)) {
        log_write(LogLevel::error, "gpu", "the window surface cannot be made current");
        release_window(device, &s);
        s = {};
        return kGpuFail;
    }
    const GpuStatus status = make_frame(&s, desc.width, desc.height);
    if (!gpu_ok(status)) {
        release_window(device, &s);
        s = {};
        return status;
    }
    s.clock = event_create(false, true);
    s.swap_interval = -1;
    s.created = true;
    out->impl = &s;
    return kGpuOk;
}

GpuStatus gpu_surface_resize(GpuSurface* surface, u32 width, u32 height) {
    GlesSurface& s = *surface->impl;
    if (width == 0 || height == 0)
        return kGpuInvalidArgument;
    if (width == s.width && height == s.height)
        return kGpuOk;
    if (s.window)
        g_wayland_egl_api.resize(s.window, static_cast<int>(width), static_cast<int>(height), 0, 0);
    s.resized = true;
    return make_frame(&s, width, height);
}

GpuRtv* gpu_surface_target(const GpuSurface* surface) {
    return surface->impl ? surface->impl->target : nullptr;
}

u32 gpu_surface_width(const GpuSurface* surface) {
    return surface->impl ? surface->impl->width : 0;
}

u32 gpu_surface_height(const GpuSurface* surface) {
    return surface->impl ? surface->impl->height : 0;
}

bool gpu_surface_valid(const GpuSurface* surface) {
    return surface->impl != nullptr;
}

GpuFormat gpu_surface_format(const GpuSurface*) {
    return GpuFormat::rgba8;
}

GpuTexture* gpu_surface_target_texture(const GpuSurface* surface) {
    const GlesSurface& s = *surface->impl;
    // The frame, or the texture a gpu_surface_set_target target was made over.
    return s.target == s.own_target ? s.frame : s.target->texture;
}

void gpu_surface_set_target(GpuSurface* surface, GpuRtv* target) {
    GlesSurface& s = *surface->impl;
    s.target = target ? target : s.own_target;
}

GpuStatus gpu_surface_present(GpuSurface* surface, u32 sync_interval, const GpuRect* dirty, u32 dirty_count) {
    (void)sync_interval; // the swap never blocks here: the frame clock paces
    GpuDevice& d = gles::device();
    GpuContext& c = gles::context();
    gl::Api& gl = d.gl;
    GlesSurface& s = *surface->impl;
    const i32 width = static_cast<i32>(s.width), height = static_cast<i32>(s.height);
    if (!s.surface) {
        // Offscreen: the frame is the product; there is nothing to blit or swap, and the next frame may start at once.
        flush_barriers();
        s.needs_full = false;
        event_set(s.clock);
        gles::poll_fences();
        return check_error("present");
    }
    if (s.swap_interval != 0) {
        d.egl.eglSwapInterval(d.display, 0);
        s.swap_interval = 0;
    }
    flush_barriers();

    // What this back buffer misses: the damage of this frame and of the frames drawn since it was last shown.
    GlesDamage now = damage_of(dirty, dirty_count, s.width, s.height);
    if (s.needs_full)
        now.full = true; // the compositor must take the whole frame as well
    GlesDamage region = now;
    egl::Int age = 0;
    if (d.caps.buffer_age)
        d.egl.eglQuerySurface(d.display, s.surface, egl::kBufferAgeExt, &age);
    const bool moved = s.offset_x != 0 || s.offset_y != 0;
    if (s.needs_full || moved || age <= 0 || static_cast<u32>(age) > s.history_count + 1) {
        region.full = true;
    } else if (!region.full) {
        for (i32 older = 0; older < age - 1 && !region.full; ++older) {
            if (s.history[older].full)
                region.full = true;
            else if (s.history[older].box.right > s.history[older].box.left)
                merge(&region.box, s.history[older].box);
        }
    }
    if (region.full)
        region.box = {0, 0, width, height};

    // The frame's row 0 is the top; the window's default framebuffer has y = 0 at the bottom: the blit flips.
    const u32 frame_fbo = texture_framebuffer(s.frame);
    if (!frame_fbo)
        return kGpuFail;
    // The window surface takes its new size at the swap after wl_egl_window_resize, not before: until then a blit is
    // clipped to the old size. So the first present after a resize blits and swaps twice, the second time into the
    // framebuffer of the right size (both go to the compositor in the same dispatch).
    bool swapped = false;
    for (u32 pass = s.resized ? 2 : 1; pass > 0; --pass) {
        gl.glDisable(gl::kScissorTest);
        gl.glBindFramebuffer(gl::kReadFramebuffer, frame_fbo);
        gl.glBindFramebuffer(gl::kDrawFramebuffer, 0);
        if (moved) {
            const float transparent[4] = {0, 0, 0, 0};
            gl.glClearBufferfv(gl::kColor, 0, transparent);
            gl.glBlitFramebuffer(0, 0, width, height, s.offset_x, height - s.offset_y, width + s.offset_x,
                                 -s.offset_y, gl::kColorBufferBit, gl::kNearest);
        } else if (region.box.right > region.box.left) {
            gl.glBlitFramebuffer(region.box.left, region.box.top, region.box.right, region.box.bottom, region.box.left,
                                 height - region.box.top, region.box.right, height - region.box.bottom,
                                 gl::kColorBufferBit, gl::kNearest);
        }
        gl.glEnable(gl::kScissorTest);
        c.applied_draw_framebuffer = 0xFFFFFFFFu;

        // The compositor learns only what changed against what it shows (this frame's damage, not the catch-up).
        if (!now.full && !moved && d.egl.eglSwapBuffersWithDamage) {
            const egl::Int rectangle[4] = {now.box.left, height - now.box.bottom, now.box.right - now.box.left,
                                           now.box.bottom - now.box.top};
            swapped = d.egl.eglSwapBuffersWithDamage(d.display, s.surface, rectangle,
                                                     now.box.right > now.box.left ? 1 : 0);
        } else {
            swapped = d.egl.eglSwapBuffers(d.display, s.surface);
        }
    }
    s.resized = false;
    for (u32 index = kGlesDamageHistory - 1; index > 0; --index)
        s.history[index] = s.history[index - 1];
    s.history[0] = now;
    if (s.history_count < kGlesDamageHistory)
        ++s.history_count;
    s.needs_full = false;
    if (!swapped) {
        log_write(LogLevel::error, "gpu", "eglSwapBuffers failed");
        log_u64(LogLevel::error, "gpu", "egl_error", static_cast<u32>(d.egl.eglGetError()));
        s.needs_full = true;
        return kGpuFail;
    }
    gles::poll_fences();
    return check_error("present");
}

GpuStatus gpu_surface_set_offset(GpuSurface* surface, float x, float y, bool* needs_present) {
    GlesSurface& s = *surface->impl;
    const i32 offset_x = static_cast<i32>(x < 0 ? x - 0.5f : x + 0.5f);
    const i32 offset_y = static_cast<i32>(y < 0 ? y - 0.5f : y + 0.5f);
    const bool changed = offset_x != s.offset_x || offset_y != s.offset_y;
    s.offset_x = offset_x;
    s.offset_y = offset_y;
    if (changed)
        s.needs_full = true;
    if (needs_present)
        *needs_present = changed;
    return kGpuOk;
}

void gpu_surface_release(GpuSurface* surface) {
    if (!surface || !surface->impl)
        return;
    GlesSurface& s = *surface->impl;
    GpuDevice& d = gles::device();
    gpu_release(s.own_target);
    gpu_release(s.frame);
    release_window(&d, &s);
    waitable_close(s.clock);
    s = {};
    surface->impl = nullptr;
}

Waitable gpu_frame_clock_waitable(const GpuSurface* surface) {
    return surface->impl ? surface->impl->clock : nullptr;
}

bool gpu_frame_try_acquire(GpuSurface* surface) {
    return wait_one(surface->impl->clock, 0) == WAIT_OBJECT_0;
}

bool gpu_frame_wait(GpuSurface* surface, u32 timeout_ms) {
    return wait_one(surface->impl->clock, timeout_ms) == WAIT_OBJECT_0;
}

void gpu_surface_frame_callback(GpuSurface* surface) {
    event_set(surface->impl->clock);
}

} // namespace maicord
