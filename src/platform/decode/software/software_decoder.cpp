#include "software_decoder.hpp"

#include "maicord/core/interlocked.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/media/video/gpu_video_frame.hpp"
#include "maicord/media/video/h264_stream.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/decode/software/abi.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../../../media/video/h264_bits.hpp"

// libdav1d and libopenh264 behind SoftwareDecoder (software_decoder.hpp): the dlopen, one decoder per stream, and the
// copy-free hand-over of the library's own picture planes.

namespace maicord {
namespace {
using namespace software;

constexpr i32 kErrorNoMemory = 12;  // ENOMEM
constexpr i32 kErrorRange = 34;     // ERANGE
constexpr u32 kAv1Queue = 4;

template <typename Api> struct Library {
    volatile LONG state; // 0 not tried, 1 loading, 2 loaded, 3 not available
    SharedLibrary library;
    Api api;
};

Library<Dav1dApi> g_dav1d;
Library<OpenH264Api> g_openh264;

void log_unavailable(const char* what) noexcept {
    log_write(LogLevel::info, "video", what);
}

bool open_first(SharedLibrary* library, const char* override_name, const char* const* names, u32 count) noexcept {
    if (override_name && *override_name)
        return shared_library_open(library, override_name);
    for (u32 i = 0; i < count; ++i)
        if (shared_library_open(library, names[i]))
            return true;
    return false;
}

bool load_dav1d() noexcept {
    static constexpr const char* names[]{"libdav1d.so.7", "libdav1d.so.6"};
    if (!open_first(&g_dav1d.library, environment_get("MAICORD_DAV1D_LIBRARY"), names, 2)) {
        log_unavailable("libdav1d is not installed: no software AV1 decode");
        return false;
    }
    Dav1dApi api{};
    bool complete = true;
#define MAICORD_DAV1D_RESOLVE(ret, name, args)                                                                         \
    api.name = reinterpret_cast<decltype(api.name)>(shared_library_symbol(g_dav1d.library, #name));                    \
    complete = complete && api.name != nullptr;
    MAICORD_DAV1D_FUNCTIONS(MAICORD_DAV1D_RESOLVE)
#undef MAICORD_DAV1D_RESOLVE
    if (!complete) {
        log_unavailable("libdav1d lacks a function: no software AV1 decode");
        shared_library_close(&g_dav1d.library);
        return false;
    }
    g_dav1d.api = api;
    return true;
}

bool load_openh264() noexcept {
    static constexpr const char* names[]{"libopenh264.so.7", "libopenh264.so.6", "libopenh264.so.8"};
    if (!open_first(&g_openh264.library, environment_get("MAICORD_OPENH264_LIBRARY"), names, 3)) {
        log_unavailable("libopenh264 is not installed: no software H.264 decode");
        return false;
    }
    OpenH264Api api{};
    api.WelsCreateDecoder = reinterpret_cast<decltype(api.WelsCreateDecoder)>(
        shared_library_symbol(g_openh264.library, "WelsCreateDecoder"));
    api.WelsDestroyDecoder = reinterpret_cast<decltype(api.WelsDestroyDecoder)>(
        shared_library_symbol(g_openh264.library, "WelsDestroyDecoder"));
    if (!api.WelsCreateDecoder || !api.WelsDestroyDecoder) {
        log_unavailable("libopenh264 lacks WelsCreateDecoder: no software H.264 decode");
        shared_library_close(&g_openh264.library);
        return false;
    }
    g_openh264.api = api;
    return true;
}

// dav1d writes decode errors to stderr by default; they reach the caller as SoftwareStatus.
void quiet_log(void*, const char*, void*) noexcept {}

template <typename Api, typename Loader> const Api* library_api(Library<Api>& slot, Loader load) noexcept {
    for (;;) {
        switch (InterlockedCompareExchange(&slot.state, 0, 0)) {
        case 2:
            return &slot.api;
        case 3:
            return nullptr;
        case 1:
            thread_sleep(1);
            continue;
        default:
            if (InterlockedCompareExchange(&slot.state, 1, 0) != 0)
                continue;
            InterlockedExchange(&slot.state, load() ? 2 : 3);
        }
    }
}

const Dav1dApi* dav1d() noexcept {
    return library_api(g_dav1d, load_dav1d);
}
const OpenH264Api* openh264() noexcept {
    return library_api(g_openh264, load_openh264);
}

} // namespace

struct SoftwareDecoder {
    GpuStreamCodec codec;
    // AV1
    Dav1dContext* av1;
    Dav1dPicture queue[kAv1Queue]; // pictures dav1d completed, oldest first
    Dav1dPicture shown;            // the picture software_decoder_pop returned last
    u32 queued;
    bool has_shown;
    // H.264
    IsvcDecoder* h264;
    SBufferInfo info;
    u8* planes[3];
    bool ready; // info holds a picture software_decoder_pop has not taken
    bool has_b_slices;
    bool reordering; // the caller orders the pictures: B slices are allowed
    // The colour description of the active SPS (matrix 2: unspecified).
    u8 matrix;
    bool full_range;
    u16 sar_width, sar_height;
};

namespace {

void av1_release_shown(SoftwareDecoder* d) noexcept {
    if (d->has_shown) {
        dav1d()->dav1d_picture_unref(&d->shown);
        d->has_shown = false;
    }
}

void av1_clear(SoftwareDecoder* d) noexcept {
    av1_release_shown(d);
    for (u32 i = 0; i < d->queued; ++i)
        dav1d()->dav1d_picture_unref(&d->queue[i]);
    d->queued = 0;
}

// Moves every completed picture into the queue (the oldest is dropped when it is full). False on a decode error.
SoftwareStatus av1_drain(SoftwareDecoder* d, bool* any) noexcept {
    const Dav1dApi& api = *dav1d();
    for (;;) {
        Dav1dPicture picture{};
        const i32 result = api.dav1d_get_picture(d->av1, &picture);
        if (result == -kDav1dEagain)
            return SoftwareStatus::ok;
        if (result < 0)
            return result == -kErrorNoMemory ? SoftwareStatus::failed : SoftwareStatus::lost;
        if (picture.bits_per_component != 8 || picture.layout != kDav1dLayoutI420 || picture.width <= 0 ||
            picture.height <= 0 || !gpu_video_frame_visible_valid(static_cast<u32>(picture.width),
                                                                  static_cast<u32>(picture.height))) {
            api.dav1d_picture_unref(&picture);
            return SoftwareStatus::unsupported;
        }
        if (d->queued == kAv1Queue) {
            api.dav1d_picture_unref(&d->queue[0]);
            for (u32 i = 1; i < kAv1Queue; ++i)
                d->queue[i - 1] = d->queue[i];
            --d->queued;
        }
        d->queue[d->queued++] = picture;
        *any = true;
    }
}

SoftwareStatus av1_push(SoftwareDecoder* d, ConstSlice<u8> unit) noexcept {
    const Dav1dApi& api = *dav1d();
    Dav1dData data{};
    u8* buffer = api.dav1d_data_create(&data, unit.count);
    if (!buffer)
        return SoftwareStatus::failed;
    copy_bytes(buffer, unit.data, unit.count);
    SoftwareStatus status = SoftwareStatus::ok;
    bool any = false;
    while (data.size && status == SoftwareStatus::ok) {
        const i32 result = api.dav1d_send_data(d->av1, &data);
        if (result < 0 && result != -kDav1dEagain) {
            status = result == -kErrorNoMemory ? SoftwareStatus::failed
                     : result == -kErrorRange  ? SoftwareStatus::unsupported
                                               : SoftwareStatus::lost;
            break;
        }
        const bool before = any;
        status = av1_drain(d, &any);
        // dav1d wants its pictures taken before it accepts more; no progress would spin forever.
        if (result == -kDav1dEagain && any == before && status == SoftwareStatus::ok) {
            status = SoftwareStatus::lost;
        }
    }
    if (data.size)
        api.dav1d_data_unref(&data);
    if (status == SoftwareStatus::ok)
        status = av1_drain(d, &any);
    if (status == SoftwareStatus::lost || status == SoftwareStatus::unsupported)
        api.dav1d_flush(d->av1);
    return status;
}

bool av1_pop(SoftwareDecoder* d, SoftwarePicture* out) noexcept {
    if (!d->queued)
        return false;
    av1_release_shown(d);
    d->shown = d->queue[0];
    d->has_shown = true;
    for (u32 i = 1; i < d->queued; ++i)
        d->queue[i - 1] = d->queue[i];
    --d->queued;
    const Dav1dPicture& p = d->shown;
    out->plane[0] = static_cast<const u8*>(p.data[0]);
    out->plane[1] = static_cast<const u8*>(p.data[1]);
    out->plane[2] = static_cast<const u8*>(p.data[2]);
    out->stride[0] = static_cast<u32>(p.stride[0]);
    out->stride[1] = out->stride[2] = static_cast<u32>(p.stride[1]);
    out->width = static_cast<u32>(p.width);
    out->height = static_cast<u32>(p.height);
    out->matrix = p.sequence ? static_cast<u8>(p.sequence->matrix) : 2;
    out->full_range = p.sequence && p.sequence->color_range;
    out->sar_width = out->sar_height = 1;
    out->timestamp = 0;
    return true;
}

// The colour description of the SPS NALs of this unit (the last one wins), and whether a slice is a B slice.
void h264_inspect(SoftwareDecoder* d, ConstSlice<u8> unit) noexcept {
    H264AnnexBCursor cursor{unit, 0};
    for (;;) {
        const H264Nal nal = h264_annexb_next(&cursor);
        if (nal.error == H264Error::end || nal.error == H264Error::invalid_input)
            return;
        if (nal.error != H264Error::none)
            continue;
        if (nal.type == 7) {
            H264Sps sps{};
            if (h264_parse_sps(nal.bytes, &sps) == H264Error::none) {
                d->matrix = sps.matrix;
                d->full_range = sps.full_range;
                d->sar_width = sps.sar_width;
                d->sar_height = sps.sar_height;
            }
        } else if ((nal.type == 1 || nal.type == 5) && nal.bytes.count > 2) {
            // first_mb_in_slice, then slice_type: 1 and 6 are B.
            H264Bits bits{{nal.bytes.data + 1, nal.bytes.count - 1}};
            bits.ue();
            const u32 type = bits.ue(9);
            if (bits.error == H264Error::none && type % 5u == 1)
                d->has_b_slices = true;
        }
    }
}

// Takes the picture libopenh264 left in d->info, if it left one.
SoftwareStatus h264_take_picture(SoftwareDecoder* d) noexcept {
    if (d->info.buffer_status == 1) {
        if (d->info.format != kVideoFormatI420 || d->info.width <= 0 || d->info.height <= 0 ||
            !gpu_video_frame_visible_valid(static_cast<u32>(d->info.width), static_cast<u32>(d->info.height)))
            return SoftwareStatus::unsupported;
        d->ready = true;
    }
    return SoftwareStatus::ok;
}

SoftwareStatus h264_push(SoftwareDecoder* d, ConstSlice<u8> unit, u64 timestamp) noexcept {
    h264_inspect(d, unit);
    // libopenh264 shows pictures in decoding order: a stream that reorders (B pictures) would be shown out of order.
    if (d->has_b_slices && !d->reordering)
        return SoftwareStatus::unsupported;
    d->ready = false;
    clear_bytes(&d->info, sizeof(d->info));
    d->info.in_timestamp = timestamp;
    d->planes[0] = d->planes[1] = d->planes[2] = nullptr;
    IsvcDecoder* decoder = d->h264;
    // A movie lets libopenh264 reorder (DecodeFrame2 holds a picture back until the stream's reordering allows it
    // out); a live stream wants each unit's picture now (DecodeFrameNoDelay).
    const i32 state = d->reordering
                          ? (*decoder)->DecodeFrame2(decoder, unit.data, static_cast<i32>(unit.count), d->planes, &d->info)
                          : (*decoder)->DecodeFrameNoDelay(decoder, unit.data, static_cast<i32>(unit.count), d->planes,
                                                           &d->info);
    if (state & (kDsInvalidArgument | kDsOutOfMemory))
        return SoftwareStatus::failed;
    // A picture with a lost reference or a concealed error is not shown: the stream waits for a keyframe.
    if (state & (kDsRefLost | kDsBitstreamError | kDsDepLayerLost | kDsNoParamSets | kDsDataErrorConcealed))
        return SoftwareStatus::lost;
    if (d->info.buffer_status != 1 && !d->reordering) {
        // Main and High streams keep the newest picture in the reordering buffer until the next unit; without B
        // pictures nothing can precede it, so it is shown now.
        clear_bytes(&d->info, sizeof(d->info));
        d->planes[0] = d->planes[1] = d->planes[2] = nullptr;
        (*decoder)->FlushFrame(decoder, d->planes, &d->info);
    }
    return h264_take_picture(d);
}

bool h264_pop(SoftwareDecoder* d, SoftwarePicture* out) noexcept {
    if (!d->ready)
        return false;
    d->ready = false;
    const u8* const* planes = d->info.planes[0] ? d->info.planes : d->planes;
    out->plane[0] = planes[0];
    out->plane[1] = planes[1];
    out->plane[2] = planes[2];
    out->stride[0] = static_cast<u32>(d->info.stride[0]);
    out->stride[1] = out->stride[2] = static_cast<u32>(d->info.stride[1]);
    out->width = static_cast<u32>(d->info.width);
    out->height = static_cast<u32>(d->info.height);
    out->matrix = d->matrix;
    out->full_range = d->full_range;
    out->sar_width = d->sar_width;
    out->sar_height = d->sar_height;
    out->timestamp = d->info.out_timestamp;
    return planes[0] && planes[1] && planes[2];
}

// The next picture libopenh264 still holds: the one of the last unit (it shows a unit's picture when the next unit
// arrives, or when it is asked to decode nothing), then the ones its reorder buffer keeps.
bool h264_drain(SoftwareDecoder* d, SoftwarePicture* out) noexcept {
    IsvcDecoder* decoder = d->h264;
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        d->ready = false;
        clear_bytes(&d->info, sizeof(d->info));
        d->planes[0] = d->planes[1] = d->planes[2] = nullptr;
        if (attempt == 0)
            (*decoder)->DecodeFrame2(decoder, nullptr, 0, d->planes, &d->info);
        else
            (*decoder)->FlushFrame(decoder, d->planes, &d->info);
        if (d->info.buffer_status == 1)
            return h264_take_picture(d) == SoftwareStatus::ok && h264_pop(d, out);
    }
    return false;
}

i32 h264_initialize(SoftwareDecoder* d) noexcept {
    SDecodingParam parameters{};
    parameters.target_dq_layer = 255;
    parameters.error_concealment = kErrorConcealmentDisable;
    parameters.video_property_size = 8; // sizeof(SVideoProperty)
    parameters.bitstream_type = kBitstreamAvc;
    return (*d->h264)->Initialize(d->h264, &parameters);
}

// A reordering decoder's pictures held back belong to the position being left, and flushing them out of
// libopenh264 one by one leaks its picture buffers (the next streams fail to find one): the decoder starts afresh.
bool h264_reopen(SoftwareDecoder* d) noexcept {
    (*d->h264)->Uninitialize(d->h264);
    return h264_initialize(d) == 0;
}

} // namespace

bool software_codec_available(GpuStreamCodec codec) noexcept {
    switch (codec) {
    case GpuStreamCodec::h264:
        return openh264() != nullptr;
    case GpuStreamCodec::av1:
        return dav1d() != nullptr;
    default:
        return false;
    }
}

SoftwareDecoder* software_decoder_create(GpuStreamCodec codec, bool reordering) noexcept {
    if (!software_codec_available(codec))
        return nullptr;
    auto* d = commit_pages<SoftwareDecoder>();
    if (!d)
        return nullptr;
    d->codec = codec;
    d->reordering = reordering;
    d->matrix = 2;
    d->sar_width = d->sar_height = 1;
    if (codec == GpuStreamCodec::av1) {
        const Dav1dApi& api = *dav1d();
        Dav1dSettings settings{};
        api.dav1d_default_settings(&settings);
        const u32 threads = processor_count() / 2u;
        settings.n_threads = static_cast<i32>(clamp_value<u32>(threads, 1, 4));
        settings.max_frame_delay = 1; // live: one picture out per unit in
        settings.apply_grain = 1;
        settings.operating_point = 0;
        settings.all_layers = 0;
        settings.frame_size_limit = static_cast<u32>(kGpuVideoVisiblePixels);
        settings.logger_callback = quiet_log;
        if (api.dav1d_open(&d->av1, &settings) < 0 || !d->av1) {
            pages_release(d, sizeof(SoftwareDecoder));
            return nullptr;
        }
        return d;
    }
    const OpenH264Api& api = *openh264();
    if (api.WelsCreateDecoder(&d->h264) != 0 || !d->h264) {
        pages_release(d, sizeof(SoftwareDecoder));
        return nullptr;
    }
    if (h264_initialize(d) != 0) {
        api.WelsDestroyDecoder(d->h264);
        pages_release(d, sizeof(SoftwareDecoder));
        return nullptr;
    }
    return d;
}

void software_decoder_destroy(SoftwareDecoder* d) noexcept {
    if (!d)
        return;
    if (d->av1) {
        av1_clear(d);
        dav1d()->dav1d_close(&d->av1);
    }
    if (d->h264) {
        (*d->h264)->Uninitialize(d->h264);
        openh264()->WelsDestroyDecoder(d->h264);
    }
    pages_release(d, sizeof(SoftwareDecoder));
}

SoftwareStatus software_decoder_push(SoftwareDecoder* d, ConstSlice<u8> unit, u64 timestamp) noexcept {
    if (!d || !unit.data || !unit.count || unit.count > 0x7fffffffu)
        return SoftwareStatus::failed;
    return d->codec == GpuStreamCodec::av1 ? av1_push(d, unit) : h264_push(d, unit, timestamp);
}

bool software_decoder_pop(SoftwareDecoder* d, SoftwarePicture* picture) noexcept {
    if (!d || !picture)
        return false;
    return d->codec == GpuStreamCodec::av1 ? av1_pop(d, picture) : h264_pop(d, picture);
}

bool software_decoder_drain(SoftwareDecoder* d, SoftwarePicture* picture) noexcept {
    if (!d || !picture || !d->reordering || !d->h264)
        return false;
    return h264_drain(d, picture);
}

void software_decoder_reset(SoftwareDecoder* d) noexcept {
    if (!d)
        return;
    if (d->av1) {
        av1_clear(d);
        dav1d()->dav1d_flush(d->av1);
    }
    // A reordering decoder holds pictures back: whatever it still has belongs to the position left.
    if (d->h264 && d->reordering)
        h264_reopen(d);
    d->ready = false;
}

} // namespace maicord
