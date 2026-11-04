#include "va.hpp"

#include "maicord/core/interlocked.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/decode/video_capability.hpp"
#include "maicord/sys/linux/abi.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord::vaapi {
namespace {

constexpr usize kCodecs = 3;
SharedLibrary g_libva, g_libva_drm;
Driver g_driver;
volatile LONG g_state; // 0 not tried, 1 opening, 2 open, 3 unavailable

void note(const char* message) noexcept {
    log_write(LogLevel::info, "video", message);
}

bool env_is(const char* name, const char* value) noexcept {
    const char* have = environment_get(name);
    if (!have)
        return false;
    u32 i = 0;
    while (have[i] && have[i] == value[i])
        ++i;
    return have[i] == value[i];
}

bool load_libraries(Api* api) noexcept {
    const char* va_name = environment_get("MAICORD_VAAPI_LIBRARY");
    const char* drm_name = environment_get("MAICORD_VAAPI_DRM_LIBRARY");
    if (!shared_library_open(&g_libva, va_name && *va_name ? va_name : "libva.so.2")) {
        note("libva.so.2 is not installed: no hardware video decode");
        return false;
    }
    if (!shared_library_open(&g_libva_drm, drm_name && *drm_name ? drm_name : "libva-drm.so.2")) {
        note("libva-drm.so.2 is not installed: no hardware video decode");
        shared_library_close(&g_libva);
        return false;
    }
    bool complete = true;
#define MAICORD_VA_RESOLVE(ret, name, args)                                                                            \
    api->name = reinterpret_cast<decltype(api->name)>(shared_library_symbol(g_libva, #name));                          \
    complete = complete && api->name != nullptr;
    MAICORD_VA_FUNCTIONS(MAICORD_VA_RESOLVE)
#undef MAICORD_VA_RESOLVE
#define MAICORD_VA_RESOLVE_DRM(ret, name, args)                                                                        \
    api->name = reinterpret_cast<decltype(api->name)>(shared_library_symbol(g_libva_drm, #name));                      \
    complete = complete && api->name != nullptr;
    MAICORD_VA_DRM_FUNCTIONS(MAICORD_VA_RESOLVE_DRM)
#undef MAICORD_VA_RESOLVE_DRM
    if (!complete) {
        note("libva lacks a function: no hardware video decode");
        shared_library_close(&g_libva_drm);
        shared_library_close(&g_libva);
    }
    return complete;
}

// Opens `path` and a VA display on it.
bool open_node(Driver* d, const char* path) noexcept {
    const long fd = sys_open(path, kOpenReadWrite | kOpenCloseOnExec);
    if (fd < 0)
        return false;
    Display display = d->api.vaGetDisplayDRM(static_cast<i32>(fd));
    i32 major = 0, minor = 0;
    if (!display || d->api.vaInitialize(display, &major, &minor) != kStatusSuccess) {
        sys_close(static_cast<int>(fd));
        return false;
    }
    d->display = display;
    d->descriptor = static_cast<i32>(fd);
    u32 n = 0;
    while (path[n] && n + 1 < sizeof(d->node)) {
        d->node[n] = path[n];
        ++n;
    }
    d->node[n] = 0;
    return true;
}

void close_node(Driver* d) noexcept {
    if (d->display)
        d->api.vaTerminate(d->display);
    if (d->descriptor >= 0)
        sys_close(d->descriptor);
    d->display = nullptr;
    d->descriptor = -1;
}

bool open_display(Driver* d, GpuDevice* gpu) noexcept {
    char node[96];
    const char* forced = environment_get("MAICORD_VAAPI_DEVICE");
    if (forced && *forced)
        return open_node(d, forced);
    if (gpu && gpu_dmabuf_render_node(gpu, node, sizeof(node)) && open_node(d, node))
        return true;
    // No node from EGL (a driver without the extension): the first render node a display initializes on.
    char path[] = "/dev/dri/renderD128";
    for (u32 index = 128; index < 192; ++index) {
        path[16] = static_cast<char>('0' + index / 100);
        path[17] = static_cast<char>('0' + index / 10 % 10);
        path[18] = static_cast<char>('0' + index % 10);
        if (open_node(d, path))
            return true;
    }
    return false;
}

constexpr i32 kCandidates[kCodecs][kProfileCandidates]{{kProfileH264High, kProfileH264Main, kProfileH264ConstrainedBaseline, 0},
                                                       {kProfileHevcMain, 0, 0, 0},
                                                       {kProfileAv1Profile0, 0, 0, 0}};

// What the driver says about one profile's VLD decoder; false when it cannot take 8-bit 4:2:0 streams in normal slice mode.
bool profile_usable(const Driver& d, i32 profile, u32* max_width, u32* max_height) noexcept {
    const i32 capacity = d.api.vaMaxNumEntrypoints(d.display);
    if (capacity <= 0 || capacity > 64)
        return false;
    i32 entrypoints[64];
    i32 count = 0;
    if (d.api.vaQueryConfigEntrypoints(d.display, profile, entrypoints, &count) != kStatusSuccess)
        return false;
    bool vld = false;
    for (i32 i = 0; i < count && i < 64; ++i)
        vld = vld || entrypoints[i] == kEntrypointVld;
    if (!vld)
        return false;
    ConfigAttrib attributes[4]{{kConfigAttribRtFormat, 0},
                               {kConfigAttribDecSliceMode, 0},
                               {kConfigAttribMaxPictureWidth, 0},
                               {kConfigAttribMaxPictureHeight, 0}};
    if (d.api.vaGetConfigAttributes(d.display, profile, kEntrypointVld, attributes, 4) != kStatusSuccess)
        return false;
    if (attributes[0].value == kAttribNotSupported || !(attributes[0].value & kRtFormatYuv420))
        return false;
    // The slice mode is optional to report; a driver that does says whether it takes whole slice parameters.
    if (attributes[1].value != kAttribNotSupported && !(attributes[1].value & kDecSliceModeNormal))
        return false;
    *max_width = attributes[2].value == kAttribNotSupported || !attributes[2].value ? 4096 : attributes[2].value;
    *max_height = attributes[3].value == kAttribNotSupported || !attributes[3].value ? 4096 : attributes[3].value;
    return true;
}

// A real surface of the driver must export as the layout the EGL import takes; otherwise the tier is of no use.
bool export_works(const Driver& d, i32 profile) noexcept {
    ConfigAttrib attribute{kConfigAttribRtFormat, kRtFormatYuv420};
    ConfigId config = kInvalidId;
    if (d.api.vaCreateConfig(d.display, profile, kEntrypointVld, &attribute, 1, &config) != kStatusSuccess)
        return false;
    SurfaceAttrib format{};
    format.type = kSurfaceAttribPixelFormat;
    format.flags = kSurfaceAttribSettable;
    format.value.type = kGenericValueInteger;
    format.value.value.i = static_cast<i32>(kFourccNv12);
    SurfaceId surface = kInvalidId;
    bool ok = d.api.vaCreateSurfaces(d.display, kRtFormatYuv420, 64, 64, &surface, 1, &format, 1) == kStatusSuccess;
    if (ok) {
        DrmPrimeDescriptor descriptor{};
        ok = d.api.vaExportSurfaceHandle(d.display, surface, kSurfaceMemoryTypeDrmPrime2,
                                         kExportSurfaceReadOnly | kExportSurfaceSeparateLayers,
                                         &descriptor) == kStatusSuccess;
        ExportLayout layout{};
        const bool laid_out = ok && export_layout(descriptor, &layout);
        for (u32 i = 0; ok && i < descriptor.num_objects && i < 4; ++i)
            if (descriptor.objects[i].fd >= 0)
                sys_close(descriptor.objects[i].fd);
        ok = laid_out;
        d.api.vaDestroySurfaces(d.display, &surface, 1);
    }
    d.api.vaDestroyConfig(d.display, config);
    return ok;
}

void probe(Driver* d) noexcept {
    d->codecs = 0;
    const i32 capacity = d->api.vaMaxNumProfiles(d->display);
    if (capacity <= 0 || capacity > 128)
        return;
    i32 profiles[128];
    i32 count = 0;
    if (d->api.vaQueryConfigProfiles(d->display, profiles, &count) != kStatusSuccess)
        return;
    i32 exported_profile = 0;
    for (usize codec = 0; codec < kCodecs; ++codec) {
        u32 found = 0;
        for (const i32 candidate : kCandidates[codec]) {
            bool listed = false;
            for (i32 i = 0; i < count && i < 128; ++i)
                listed = listed || profiles[i] == candidate;
            u32 width = 0, height = 0;
            if (!candidate || !listed || !profile_usable(*d, candidate, &width, &height))
                continue;
            d->profiles[codec][found++] = candidate;
            d->max_width[codec] = width;
            d->max_height[codec] = height;
            if (!exported_profile)
                exported_profile = candidate;
        }
        if (found)
            d->codecs |= 1u << codec;
    }
    // One real surface proves the export path for every codec of this driver.
    if (d->codecs && !export_works(*d, exported_profile)) {
        note("VA-API surfaces do not export as per-plane dma-bufs: no hardware video decode");
        d->codecs = 0;
    }
}

} // namespace

bool export_layout(const DrmPrimeDescriptor& d, ExportLayout* layout) noexcept {
    const bool ten_bit = d.fourcc == kFourccP010;
    if ((d.fourcc != kFourccNv12 && !ten_bit) || d.num_layers != 2 || !d.num_objects || d.num_objects > 4 || !d.width ||
        !d.height)
        return false;
    ExportLayout result{};
    result.objects = d.num_objects;
    for (u32 i = 0; i < 2; ++i) {
        const auto& layer = d.layers[i];
        const u32 expected = i == 0 ? (ten_bit ? kDrmFormatR16 : kDrmFormatR8) : (ten_bit ? kDrmFormatGr1616 : kDrmFormatGr88);
        if (layer.drm_format != expected || layer.num_planes != 1 || layer.object_index[0] >= d.num_objects ||
            !layer.pitch[0])
            return false;
        const auto& object = d.objects[layer.object_index[0]];
        if (object.fd < 0)
            return false;
        result.plane[i] = {object.fd,
                           expected,
                           i == 0 ? d.width : (d.width + 1) / 2,
                           i == 0 ? d.height : (d.height + 1) / 2,
                           layer.offset[0],
                           layer.pitch[0],
                           object.drm_format_modifier};
    }
    *layout = result;
    return true;
}

const Driver* driver(GpuDevice* gpu) noexcept {
    for (;;) {
        switch (InterlockedCompareExchange(&g_state, 0, 0)) {
        case 2:
            return &g_driver;
        case 3:
            return nullptr;
        case 1:
            thread_sleep(1);
            continue;
        default:
            if (InterlockedCompareExchange(&g_state, 1, 0) != 0)
                continue;
            bool open = false;
            if (env_is("MAICORD_VAAPI", "off")) {
                note("MAICORD_VAAPI=off: no hardware video decode");
            } else if (load_libraries(&g_driver.api)) {
                if (!open_display(&g_driver, gpu)) {
                    note("no VA-API display on a render node: no hardware video decode");
                    shared_library_close(&g_libva_drm);
                    shared_library_close(&g_libva);
                } else {
                    probe(&g_driver);
                    open = g_driver.codecs != 0;
                    if (open && !(gpu && gpu_caps(gpu).dmabuf_import)) {
                        note("the GPU cannot import dma-bufs: no hardware video decode");
                        open = false;
                    }
                    if (open) {
                        log_u64(LogLevel::info, "video", "vaapi_codecs", g_driver.codecs);
                    } else {
                        close_node(&g_driver);
                        shared_library_close(&g_libva_drm);
                        shared_library_close(&g_libva);
                    }
                }
            }
            InterlockedExchange(&g_state, open ? 2 : 3);
        }
    }
}

i32 h264_profile_for(const Driver& d, u8 profile_idc) noexcept {
    // The lowest profile of the driver that contains the stream's: Constrained Baseline streams fit all three, Main
    // fits Main and High, anything of the High family needs High.
    static constexpr i32 order[]{kProfileH264ConstrainedBaseline, kProfileH264Main, kProfileH264High};
    const u32 first = profile_idc == 66 ? 0 : profile_idc == 77 ? 1 : 2;
    for (u32 i = first; i < 3; ++i)
        for (const i32 have : d.profiles[0])
            if (have == order[i])
                return order[i];
    return 0;
}

Session* session_create(const Driver* d, i32 profile, u32 width, u32 height, u32 count) noexcept {
    if (!d || !count || count > kSurfaceLimit || !width || !height)
        return nullptr;
    auto* session = commit_pages<Session>();
    if (!session)
        return nullptr;
    session->driver = d;
    session->profile = profile;
    session->width = width;
    session->height = height;
    ConfigAttrib attribute{kConfigAttribRtFormat, kRtFormatYuv420};
    SurfaceAttrib format{};
    format.type = kSurfaceAttribPixelFormat;
    format.flags = kSurfaceAttribSettable;
    format.value.type = kGenericValueInteger;
    format.value.value.i = static_cast<i32>(kFourccNv12);
    session->config = session->context = kInvalidId;
    bool ok = d->api.vaCreateConfig(d->display, profile, kEntrypointVld, &attribute, 1, &session->config) ==
              kStatusSuccess;
    if (ok)
        ok = d->api.vaCreateSurfaces(d->display, kRtFormatYuv420, width, height, session->surface, count, &format, 1) ==
             kStatusSuccess;
    if (ok) {
        session->count = count;
        ok = d->api.vaCreateContext(d->display, session->config, static_cast<i32>(width), static_cast<i32>(height),
                                    kProgressive, session->surface, static_cast<i32>(count), &session->context) ==
             kStatusSuccess;
    }
    if (!ok) {
        session_destroy(session);
        return nullptr;
    }
    return session;
}

void session_destroy(Session* session) noexcept {
    if (!session)
        return;
    const Driver* d = session->driver;
    if (session->context != kInvalidId)
        d->api.vaDestroyContext(d->display, session->context);
    if (session->count)
        d->api.vaDestroySurfaces(d->display, session->surface, static_cast<i32>(session->count));
    if (session->config != kInvalidId)
        d->api.vaDestroyConfig(d->display, session->config);
    pages_release(session, sizeof(Session));
}

namespace {

BufferId make_buffer(Session* s, i32 type, const void* data, u32 bytes) noexcept {
    BufferId id = kInvalidId;
    if (s->created_count == sizeof(s->created) / sizeof(s->created[0]) ||
        s->driver->api.vaCreateBuffer(s->driver->display, s->context, type, bytes, 1, const_cast<void*>(data), &id) !=
            kStatusSuccess)
        return kInvalidId;
    s->created[s->created_count++] = id;
    return id;
}

bool flush_slices(Session* s) noexcept {
    if (!s->pending_count)
        return true;
    const bool ok = s->driver->api.vaRenderPicture(s->driver->display, s->context, s->pending,
                                                   static_cast<i32>(s->pending_count)) == kStatusSuccess;
    s->pending_count = 0;
    return ok;
}

} // namespace

bool picture_begin(Session* s, u32 surface) noexcept {
    if (!s || surface >= s->count)
        return false;
    s->created_count = 0;
    s->pending_count = 0;
    s->failed = false;
    s->target = s->surface[surface];
    s->open = s->driver->api.vaBeginPicture(s->driver->display, s->context, s->target) == kStatusSuccess;
    s->failed = !s->open;
    return s->open;
}

bool picture_buffer(Session* s, i32 type, const void* data, u32 bytes) noexcept {
    if (!s->open || s->failed)
        return false;
    BufferId id = make_buffer(s, type, data, bytes);
    if (id == kInvalidId || s->driver->api.vaRenderPicture(s->driver->display, s->context, &id, 1) != kStatusSuccess)
        s->failed = true;
    return !s->failed;
}

namespace {

bool pend(Session* s, i32 type, const void* data, u32 bytes) noexcept {
    if (!s->open || s->failed)
        return false;
    // A slice's parameter and data buffers go out in the same vaRenderPicture: flush only before a parameter buffer.
    if (type == kSliceParameterBuffer && s->pending_count + 2 > sizeof(s->pending) / sizeof(s->pending[0]) && !flush_slices(s)) {
        s->failed = true;
        return false;
    }
    const BufferId id = make_buffer(s, type, data, bytes);
    if (id == kInvalidId) {
        s->failed = true;
        return false;
    }
    s->pending[s->pending_count++] = id;
    return true;
}

} // namespace

bool picture_slice_parameters(Session* s, const void* parameters, u32 bytes) noexcept {
    return pend(s, kSliceParameterBuffer, parameters, bytes);
}

bool picture_slice_data(Session* s, const void* data, u32 bytes) noexcept {
    return pend(s, kSliceDataBuffer, data, bytes);
}

bool picture_end(Session* s) noexcept {
    if (!s->open)
        return false;
    const Driver* d = s->driver;
    bool ok = !s->failed && flush_slices(s);
    // vaEndPicture must follow vaBeginPicture whether or not the buffers were accepted.
    ok = d->api.vaEndPicture(d->display, s->context) == kStatusSuccess && ok;
    for (u32 i = 0; i < s->created_count; ++i)
        d->api.vaDestroyBuffer(d->display, s->created[i]);
    s->created_count = s->pending_count = 0;
    s->open = false;
    if (ok)
        ok = d->api.vaSyncSurface(d->display, s->target) == kStatusSuccess;
    return ok;
}

bool export_picture(GpuDevice* gpu, Session* s, u32 surface, ExportedPicture* picture) noexcept {
    *picture = {};
    if (!s || surface >= s->count)
        return false;
    const Driver* d = s->driver;
    DrmPrimeDescriptor descriptor{};
    if (d->api.vaExportSurfaceHandle(d->display, s->surface[surface], kSurfaceMemoryTypeDrmPrime2,
                                     kExportSurfaceReadOnly | kExportSurfaceSeparateLayers, &descriptor) !=
        kStatusSuccess)
        return false;
    ExportLayout layout{};
    bool ok = export_layout(descriptor, &layout);
    if (ok)
        ok = gpu_ok(gpu_video_plane_import(gpu, layout.plane[0], &picture->luma_texture, &picture->luma_view));
    if (ok)
        ok = gpu_ok(gpu_video_plane_import(gpu, layout.plane[1], &picture->chroma_texture, &picture->chroma_view));
    // The images hold their own references to the buffers.
    for (u32 i = 0; i < descriptor.num_objects && i < 4; ++i)
        if (descriptor.objects[i].fd >= 0)
            sys_close(descriptor.objects[i].fd);
    if (!ok) {
        gpu_release(picture->luma_view);
        gpu_release(picture->luma_texture);
        gpu_release(picture->chroma_view);
        gpu_release(picture->chroma_texture);
    }
    return ok;
}

} // namespace maicord::vaapi
