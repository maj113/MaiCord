// The decoded-picture planes of the OpenGL ES backend (maicord/gpu/video_planes.hpp): a fixed pool of planes, each a
// GpuTexture and a GpuSrv with an atomic count, made from a dma-buf's EGLImage or from CPU memory without GL, and
// realized as a GL texture on the renderer's thread.
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/gpu/video_planes.hpp"
#include "gpu/gles/internal.hpp"
#include "gpu/gles/video_planes_internal.hpp"

namespace maicord {
namespace gles {
namespace {

constexpr i32 kEglNone = 0x3038;
constexpr i32 kEglWidth = 0x3057;
constexpr i32 kEglHeight = 0x3056;
constexpr u32 kEglLinuxDmaBuf = 0x3270;     // EGL_LINUX_DMA_BUF_EXT
constexpr i32 kEglLinuxDrmFourcc = 0x3271;  // EGL_LINUX_DRM_FOURCC_EXT
constexpr i32 kEglPlane0Fd = 0x3272;        // EGL_DMA_BUF_PLANE0_FD_EXT
constexpr i32 kEglPlane0Offset = 0x3273;    // EGL_DMA_BUF_PLANE0_OFFSET_EXT
constexpr i32 kEglPlane0Pitch = 0x3274;     // EGL_DMA_BUF_PLANE0_PITCH_EXT
constexpr i32 kEglPlane0ModifierLow = 0x3443;  // EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
constexpr i32 kEglPlane0ModifierHigh = 0x3444; // EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT
constexpr i32 kEglDeviceExt = 0x322C;          // EGL_DEVICE_EXT
constexpr i32 kEglDrmDeviceFile = 0x3233;      // EGL_DRM_DEVICE_FILE_EXT
constexpr i32 kEglDrmRenderNodeFile = 0x3377;  // EGL_DRM_RENDER_NODE_FILE_EXT
constexpr u32 kGlR16 = 0x822A;                 // GL_R16_EXT
constexpr u32 kGlRg16 = 0x822C;                // GL_RG16_EXT

enum PlaneKind : u8 { plane_free, plane_cpu, plane_dmabuf };

struct VideoPlane {
    GpuTexture texture;
    GpuSrv view;
    volatile LONG refs;       // owners: each handle made, every retain
    volatile LONG generation; // CPU planes: bumped by gpu_video_plane_publish
    u32 uploaded;             // renderer thread: the generation in the GL texture
    void* image;              // EGLImage of a dma-buf plane
    u8* pixels;               // CPU plane memory
    u32 pixel_bytes;
    u32 pitch;
    u32 next_retired; // index + 1 of the next retired plane
    u8 kind;
    u8 channels;
    bool retired;
};

VideoPlane g_planes[kGpuVideoPlaneCapacity];
volatile LONG g_lock;
u32 g_retired; // index + 1 of the newest retired plane whose GL texture awaits deletion
volatile LONG g_api_state; // 0 not resolved, 1 resolving, 2 resolved

struct ImageApi {
    void* (*create_khr)(void* display, void* context, u32 target, void* buffer, const i32* attributes);
    void* (*create_core)(void* display, void* context, u32 target, void* buffer, const i64* attributes);
    u32 (*destroy_khr)(void* display, void* image);
    u32 (*destroy_core)(void* display, void* image);
    void (*target_texture)(u32 target, void* image);
    u32 (*query_display_attribute)(void* display, i32 attribute, i64* value);
    const char* (*query_device_string)(void* device, i32 name);
    u32 (*query_modifiers)(void* display, i32 format, i32 max, u64* modifiers, u32* external_only, i32* count);
    bool modifiers;
};
ImageApi g_images;

void lock() noexcept {
    while (InterlockedExchange(&g_lock, 1)) {
        while (g_lock)
            __builtin_ia32_pause();
    }
}
void unlock() noexcept {
    InterlockedExchange(&g_lock, 0);
}

// Resolves the extension entry points once; false when the device cannot import dma-bufs.
bool resolve_images(GpuDevice& device) noexcept {
    for (;;) {
        switch (InterlockedCompareExchange(&g_api_state, 0, 0)) {
        case 2:
            return g_images.target_texture && (g_images.create_khr || g_images.create_core);
        case 1:
            while (g_api_state == 1)
                __builtin_ia32_pause();
            continue;
        default:
            if (InterlockedCompareExchange(&g_api_state, 1, 0) != 0)
                continue;
            const auto load = [&](const char* name) noexcept { return device.egl.eglGetProcAddress(name); };
            ImageApi api{};
            api.create_khr = reinterpret_cast<decltype(api.create_khr)>(load("eglCreateImageKHR"));
            api.destroy_khr = reinterpret_cast<decltype(api.destroy_khr)>(load("eglDestroyImageKHR"));
            api.create_core = reinterpret_cast<decltype(api.create_core)>(load("eglCreateImage"));
            api.destroy_core = reinterpret_cast<decltype(api.destroy_core)>(load("eglDestroyImage"));
            api.target_texture = reinterpret_cast<decltype(api.target_texture)>(load("glEGLImageTargetTexture2DOES"));
            api.query_display_attribute =
                reinterpret_cast<decltype(api.query_display_attribute)>(load("eglQueryDisplayAttribEXT"));
            api.query_device_string = reinterpret_cast<decltype(api.query_device_string)>(load("eglQueryDeviceStringEXT"));
            api.query_modifiers = reinterpret_cast<decltype(api.query_modifiers)>(load("eglQueryDmaBufModifiersEXT"));
            const char* extensions = device.egl.eglQueryString(device.display, egl::kExtensions);
            api.modifiers = string_has_word(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
            if (!api.destroy_khr && !api.destroy_core) {
                api.create_khr = nullptr;
                api.create_core = nullptr;
            }
            g_images = api;
            InterlockedExchange(&g_api_state, 2);
        }
    }
}

VideoPlane* plane_of(const void* object) noexcept {
    const auto* base = reinterpret_cast<const u8*>(g_planes);
    const auto* at = static_cast<const u8*>(object);
    if (at < base || at >= base + sizeof(g_planes))
        return nullptr;
    return &g_planes[static_cast<usize>(at - base) / sizeof(VideoPlane)];
}

VideoPlane* allocate_plane(u8 kind, u32 width, u32 height, GpuFormat format) noexcept {
    lock();
    VideoPlane* plane = nullptr;
    for (VideoPlane& candidate : g_planes)
        if (candidate.kind == plane_free && !candidate.retired) {
            plane = &candidate;
            break;
        }
    if (plane) {
        clear_bytes(plane, sizeof(*plane));
        plane->kind = kind; // taken before the lock goes
    }
    unlock();
    if (!plane)
        return nullptr;
    plane->refs = 2; // the texture handle and the view handle the creator gets
    plane->texture.refs = 1;
    plane->texture.width = width;
    plane->texture.height = height;
    plane->texture.internal = format == GpuFormat::rg8    ? gl::kRg8
                              : format == GpuFormat::r16  ? kGlR16
                              : format == GpuFormat::rg16 ? kGlRg16
                                                          : gl::kR8;
    plane->texture.format = format;
    plane->texture.bind = gpu_bind_srv;
    plane->view.kind = GpuViewKind::texture;
    plane->view.texture = &plane->texture;
    return plane;
}

// Frees what a plane owns that needs no GL, then the slot.
void free_plane(VideoPlane* plane) noexcept {
    if (plane->image) {
        GpuDevice& d = device();
        if (g_images.destroy_khr)
            g_images.destroy_khr(d.display, plane->image);
        else if (g_images.destroy_core)
            g_images.destroy_core(d.display, plane->image);
    }
    if (plane->pixels)
        pages_release(plane->pixels, plane->pixel_bytes);
    lock();
    clear_bytes(plane, sizeof(*plane)); // kind is plane_free again
    unlock();
}

void retire_plane(VideoPlane* plane) noexcept {
    if (plane->texture.name) {
        // The GL texture belongs to the renderer's thread: queue it.
        lock();
        plane->retired = true;
        plane->next_retired = g_retired;
        g_retired = static_cast<u32>(plane - g_planes) + 1;
        unlock();
        return;
    }
    free_plane(plane);
}

void forget_unit_texture(GpuContext& c, u32 name) noexcept {
    for (u32& applied : c.applied_unit_texture)
        if (applied == name)
            applied = 0xFFFFFFFFu;
}

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// The modifier is listed for the format by eglQueryDmaBufModifiersEXT and can be sampled as a plain 2D texture (not
// external-only). One query per import: a decoder on a driver whose exports are not kept pays it for every plane.
bool modifier_usable(GpuDevice& device, u32 fourcc, u64 modifier) noexcept {
    constexpr i32 kCapacity = 256;
    u64 modifiers[kCapacity];
    u32 external_only[kCapacity];
    i32 returned = 0;
    bool usable = false;
    if (g_images.query_modifiers(device.display, static_cast<i32>(fourcc), kCapacity, modifiers, external_only, &returned))
        for (i32 i = 0; i < returned && i < kCapacity; ++i)
            if (modifiers[i] == modifier)
                usable = !external_only[i];
    if (!usable)
        log_u64(LogLevel::warning, "video", "dmabuf_modifier_unusable", modifier);
    return usable;
}
#pragma clang attribute pop

} // namespace

bool video_plane_retain(const void* object) noexcept {
    VideoPlane* plane = plane_of(object);
    if (!plane)
        return false;
    InterlockedIncrement(&plane->refs);
    return true;
}

bool video_plane_release(const void* object) noexcept {
    VideoPlane* plane = plane_of(object);
    if (!plane)
        return false;
    if (InterlockedDecrement(&plane->refs) == 0)
        retire_plane(plane);
    return true;
}

bool video_plane_realize(const void* object) noexcept {
    VideoPlane* plane = plane_of(object);
    if (!plane || plane->kind == plane_free)
        return false;
    GpuDevice& d = device();
    GpuContext& c = context();
    gl::Api& gl = d.gl;
    const bool cpu = plane->kind == plane_cpu;
    const bool create = plane->texture.name == 0;
    const bool upload = cpu && static_cast<u32>(plane->generation) != plane->uploaded;
    if (!create && !upload)
        return true;
    if (!cpu && create && !resolve_images(d))
        return false;
    // Unit 0 is borrowed: its binding is put back as the state tracker believes it is.
    const u32 saved = c.applied_unit_texture[0];
    u32 name = plane->texture.name;
    if (create)
        gl.glGenTextures(1, &name);
    gl.glActiveTexture(gl::kTexture0);
    gl.glBindTexture(gl::kTexture2d, name);
    bool ok = true;
    if (create) {
        gl.glTexParameteri(gl::kTexture2d, gl::kTextureMinFilter, gl::kNearest);
        gl.glTexParameteri(gl::kTexture2d, gl::kTextureMagFilter, gl::kNearest);
        gl.glTexParameteri(gl::kTexture2d, gl::kTextureWrapS, gl::kClampToEdge);
        gl.glTexParameteri(gl::kTexture2d, gl::kTextureWrapT, gl::kClampToEdge);
        if (cpu)
            gl.glTexStorage2D(gl::kTexture2d, 1, plane->texture.internal, static_cast<gl::Sizei>(plane->texture.width),
                              static_cast<gl::Sizei>(plane->texture.height));
        else
            g_images.target_texture(gl::kTexture2d, plane->image);
        ok = check_error("video plane") == kGpuOk;
    }
    if (ok && upload) {
        // Rows are tight: the device's unpack alignment is 1.
        const u32 generation = static_cast<u32>(plane->generation);
        gl.glTexSubImage2D(gl::kTexture2d, 0, 0, 0, static_cast<gl::Sizei>(plane->texture.width),
                           static_cast<gl::Sizei>(plane->texture.height), plane->channels == 2 ? gl::kRg : gl::kRed,
                           gl::kUnsignedByte, plane->pixels);
        plane->uploaded = generation;
        ok = check_error("video plane upload") == kGpuOk;
    }
    if (saved != 0xFFFFFFFFu)
        gl.glBindTexture(gl::kTexture2d, saved);
    c.applied_unit_texture[0] = saved;
    if (create) {
        if (ok) {
            plane->texture.name = name;
        } else {
            gl.glDeleteTextures(1, &name);
            forget_unit_texture(c, name);
        }
    }
    return ok;
}

void video_planes_shutdown() noexcept {
    for (VideoPlane& plane : g_planes)
        if (plane.kind != plane_free)
            free_plane(&plane);
    g_retired = 0;
    g_images = {};
    InterlockedExchange(&g_api_state, 0);
}

} // namespace gles

// The import of a decoded picture's plane: once per plane (per picture on a driver whose exports are not kept), the
// EGL calls dominate it: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
u32 gpu_dmabuf_attributes(const GpuDmabufPlane& plane, bool modifiers, i32* attributes) noexcept {
    u32 n = 0;
    const auto put = [&](i32 key, i32 value) noexcept {
        attributes[n++] = key;
        attributes[n++] = value;
    };
    put(gles::kEglWidth, static_cast<i32>(plane.width));
    put(gles::kEglHeight, static_cast<i32>(plane.height));
    put(gles::kEglLinuxDrmFourcc, static_cast<i32>(plane.fourcc));
    // Every plane of an image names the same modifier.
    const bool modifier = modifiers && plane.modifier != kDrmFormatModifierInvalid;
    const auto put_plane = [&](i32 index, i32 fd, u32 offset, u32 pitch) noexcept {
        put(gles::kEglPlane0Fd + 3 * index, fd);
        put(gles::kEglPlane0Offset + 3 * index, static_cast<i32>(offset));
        put(gles::kEglPlane0Pitch + 3 * index, static_cast<i32>(pitch));
        if (modifier) {
            put(gles::kEglPlane0ModifierLow + 2 * index, static_cast<i32>(plane.modifier & 0xFFFFFFFFu));
            put(gles::kEglPlane0ModifierHigh + 2 * index, static_cast<i32>(plane.modifier >> 32));
        }
    };
    put_plane(0, plane.fd, plane.offset, plane.pitch);
    if (plane.aux_pitch)
        put_plane(1, plane.aux_fd, plane.aux_offset, plane.aux_pitch);
    attributes[n++] = gles::kEglNone;
    return n;
}

bool gpu_dmabuf_plane_supported(GpuDevice* device, const GpuDmabufPlane& plane) {
    if (!device || !device->caps.dmabuf_import || !gles::resolve_images(*device))
        return false;
    // A linear buffer is what an import without modifiers assumes, and an unspecified modifier leaves the layout to the
    // driver: neither needs a listing, and neither has a second plane.
    if (plane.modifier == kDrmFormatModifierLinear || plane.modifier == kDrmFormatModifierInvalid)
        return !plane.aux_pitch;
    if (!gles::g_images.modifiers)
        return false; // a tiled buffer would be read as a linear one
    if (!gles::g_images.query_modifiers)
        return true; // the extension is there without its query: the import itself says
    return gles::modifier_usable(*device, plane.fourcc, plane.modifier);
}

GpuStatus gpu_video_plane_import(GpuDevice* device, const GpuDmabufPlane& source, GpuTexture** texture, GpuSrv** view) {
    if (texture)
        *texture = nullptr;
    if (view)
        *view = nullptr;
    if (!device || !texture || !view || source.fd < 0 || !source.width || !source.height || source.pitch == 0 ||
        source.width > 16384 || source.height > 16384 || (source.aux_pitch && source.aux_fd < 0))
        return kGpuInvalidArgument;
    const bool chroma = source.fourcc == kDrmFormatGr88 || source.fourcc == kDrmFormatGr1616;
    const bool wide = source.fourcc == kDrmFormatR16 || source.fourcc == kDrmFormatGr1616;
    if (!chroma && source.fourcc != kDrmFormatR8 && source.fourcc != kDrmFormatR16)
        return kGpuInvalidArgument;
    if (!gpu_dmabuf_plane_supported(device, source))
        return kGpuUnsupported;
    i32 attributes[32]{};
    gpu_dmabuf_attributes(source, gles::g_images.modifiers, attributes);
    void* image = nullptr;
    if (gles::g_images.create_khr) {
        image = gles::g_images.create_khr(device->display, nullptr, gles::kEglLinuxDmaBuf, nullptr, attributes);
    } else {
        i64 wide_attributes[32];
        for (u32 i = 0; i < 32; ++i)
            wide_attributes[i] = attributes[i];
        image = gles::g_images.create_core(device->display, nullptr, gles::kEglLinuxDmaBuf, nullptr, wide_attributes);
    }
    if (!image) {
        log_u64(LogLevel::warning, "video", "dmabuf_import_failed_egl", static_cast<u64>(device->egl.eglGetError()));
        return kGpuFail;
    }
    const GpuFormat format = wide ? (chroma ? GpuFormat::rg16 : GpuFormat::r16) : (chroma ? GpuFormat::rg8 : GpuFormat::r8);
    gles::VideoPlane* plane = gles::allocate_plane(gles::plane_dmabuf, source.width, source.height, format);
    if (!plane) {
        if (gles::g_images.destroy_khr)
            gles::g_images.destroy_khr(device->display, image);
        else
            gles::g_images.destroy_core(device->display, image);
        return kGpuOutOfMemory;
    }
    plane->image = image;
    plane->channels = chroma ? 2 : 1;
    *texture = &plane->texture;
    *view = &plane->view;
    return kGpuOk;
}

#pragma clang attribute pop

GpuStatus gpu_video_plane_create(GpuDevice* device, GpuVideoPlaneFormat format, u32 width, u32 height,
                                 GpuTexture** texture, GpuSrv** view, u8** pixels, u32* pitch) {
    if (texture)
        *texture = nullptr;
    if (view)
        *view = nullptr;
    if (!device || !texture || !view || !pixels || !pitch || !width || !height || width > 16384 || height > 16384)
        return kGpuInvalidArgument;
    const u32 channels = format == GpuVideoPlaneFormat::rg8 ? 2 : 1;
    const u64 bytes = (u64{width} * channels * height + 4095u) & ~u64{4095};
    u8* memory = static_cast<u8*>(commit_pages(bytes));
    if (!memory)
        return kGpuOutOfMemory;
    gles::VideoPlane* plane = gles::allocate_plane(gles::plane_cpu, width, height,
                                                   channels == 2 ? GpuFormat::rg8 : GpuFormat::r8);
    if (!plane) {
        pages_release(memory, bytes);
        return kGpuOutOfMemory;
    }
    plane->pixels = memory;
    plane->pixel_bytes = static_cast<u32>(bytes);
    plane->pitch = width * channels;
    plane->channels = static_cast<u8>(channels);
    *texture = &plane->texture;
    *view = &plane->view;
    *pixels = memory;
    *pitch = plane->pitch;
    return kGpuOk;
}

void gpu_video_plane_publish(GpuTexture* texture) {
    gles::VideoPlane* plane = gles::plane_of(texture);
    if (plane && plane->kind == gles::plane_cpu)
        InterlockedIncrement(&plane->generation);
}

bool gpu_video_plane_owns(const GpuTexture* texture) {
    return gles::plane_of(texture) != nullptr;
}

void gpu_video_planes_reap(GpuDevice* device) {
    if (!device)
        return;
    gles::lock();
    u32 head = gles::g_retired;
    gles::g_retired = 0;
    gles::unlock();
    GpuContext& c = gles::context();
    while (head) {
        gles::VideoPlane* plane = &gles::g_planes[head - 1];
        head = plane->next_retired;
        if (plane->texture.name) {
            device->gl.glDeleteTextures(1, &plane->texture.name);
            gles::forget_unit_texture(c, plane->texture.name);
        }
        gles::free_plane(plane);
    }
}

bool gpu_dmabuf_render_node(GpuDevice* device, char* path, u32 capacity) {
    if (!device || !path || capacity < 64 || !gles::resolve_images(*device))
        return false;
    const auto& images = gles::g_images;
    if (!images.query_display_attribute || !images.query_device_string)
        return false;
    i64 attribute = 0;
    if (!images.query_display_attribute(device->display, gles::kEglDeviceExt, &attribute) || !attribute)
        return false;
    void* egl_device = reinterpret_cast<void*>(attribute);
    const char* node = images.query_device_string(egl_device, gles::kEglDrmRenderNodeFile);
    if (!node || !*node)
        node = images.query_device_string(egl_device, gles::kEglDrmDeviceFile);
    if (!node || !*node)
        return false;
    return node[c_string_copy(path, capacity, node)] == 0;
}

} // namespace maicord
