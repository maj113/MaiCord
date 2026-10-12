#include "mp4_ffmpeg_gpu_reference.hpp"

#if defined(MAICORD_FFMPEG_ROOT)
#include "maicord/core/memory.hpp"
#include <d3d11.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace maicord {
namespace {
// Resolve only when the FFmpeg backend is selected. Importing its DLLs into
// every benchmark process would contaminate Direct/MF startup and memory data.
// Modules live for this short-lived process; warm runs reuse the same modules.
#define FF_UTIL(X) \
    X(avutil_version) X(av_malloc) X(av_free) X(av_frame_alloc) \
    X(av_frame_free) X(av_frame_unref) X(av_buffer_ref) X(av_buffer_unref) \
    X(av_hwdevice_ctx_alloc) X(av_hwdevice_ctx_init) X(av_rescale_q)
#define FF_CODEC(X) \
    X(avcodec_version) X(avcodec_find_decoder) X(avcodec_alloc_context3) \
    X(avcodec_parameters_to_context) X(avcodec_open2) X(avcodec_free_context) \
    X(avcodec_send_packet) X(avcodec_receive_frame) X(avcodec_flush_buffers) \
    X(av_packet_alloc) X(av_packet_free) X(av_packet_unref)
#define FF_FORMAT(X) \
    X(avformat_version) X(avformat_alloc_context) X(avformat_open_input) \
    X(avformat_find_stream_info) X(av_find_best_stream) X(avformat_close_input) \
    X(avio_alloc_context) X(avio_context_free) X(av_read_frame) X(av_seek_frame)
struct Api {
#define FF_FIELD(name) decltype(&name) name;
    FF_UTIL(FF_FIELD) FF_CODEC(FF_FIELD) FF_FORMAT(FF_FIELD)
#undef FF_FIELD
    bool ready;
};
Api api{};
DWORD load_error = ERROR_SUCCESS;
template<u32 N> HMODULE load_module(const wchar_t (&name)[N]) noexcept {
    wchar_t path[N];
    for (u32 index = 0; index < N; ++index) path[index] = name[index] == L'/' ? L'\\' : name[index];
    const HMODULE module = LoadLibraryExW(path, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) load_error = GetLastError();
    return module;
}
bool load_api() noexcept {
    if (api.ready) return true;
    const HMODULE util = load_module(MAICORD_FFMPEG_ROOT L"/bin/avutil-61.dll");
    const HMODULE codec = load_module(MAICORD_FFMPEG_ROOT L"/bin/avcodec-63.dll");
    const HMODULE format = load_module(MAICORD_FFMPEG_ROOT L"/bin/avformat-63.dll");
    bool ok = util && codec && format;
#define FF_LOAD(module, name) api.name = reinterpret_cast<decltype(api.name)>(GetProcAddress(module, #name)); if (!api.name) load_error = GetLastError(); ok = ok && api.name;
#define FF_LOAD_UTIL(name) FF_LOAD(util, name)
#define FF_LOAD_CODEC(name) FF_LOAD(codec, name)
#define FF_LOAD_FORMAT(name) FF_LOAD(format, name)
    if (ok) {
        FF_UTIL(FF_LOAD_UTIL) FF_CODEC(FF_LOAD_CODEC) FF_FORMAT(FF_LOAD_FORMAT)
    }
#undef FF_LOAD_FORMAT
#undef FF_LOAD_CODEC
#undef FF_LOAD_UTIL
#undef FF_LOAD
    ok = ok && (api.avutil_version() >> 16u) == LIBAVUTIL_VERSION_MAJOR &&
        (api.avcodec_version() >> 16u) == LIBAVCODEC_VERSION_MAJOR &&
        (api.avformat_version() >> 16u) == LIBAVFORMAT_VERSION_MAJOR;
    if (!ok) {
        if (format) FreeLibrary(format);
        if (codec) FreeLibrary(codec);
        if (util) FreeLibrary(util);
        api = {};
        if (!load_error) load_error = ERROR_REVISION_MISMATCH;
    } else { api.ready = true; }
    return ok;
}
#undef FF_FORMAT
#undef FF_CODEC
#undef FF_UTIL

struct State {
    ConstSlice<u8> encoded;
    u64 cursor;
    AVFormatContext* format;
    AVIOContext* io;
    AVCodecContext* codec;
    AVPacket* packet;
    AVFrame* frame;
    AVBufferRef* device;
    i64 previous_pts;
    int stream;
    bool draining;
};
int read_bytes(void* opaque, u8* destination, int capacity) noexcept {
    auto* s = static_cast<State*>(opaque);
    if (capacity <= 0) return AVERROR(EINVAL);
    if (s->cursor == s->encoded.count) return AVERROR_EOF;
    const u32 count = static_cast<u32>(minimum<u64>(s->encoded.count - s->cursor, static_cast<u32>(capacity)));
    copy_bytes(destination, s->encoded.data + s->cursor, count);
    s->cursor += count;
    return static_cast<int>(count);
}
int64_t seek_bytes(void* opaque, int64_t offset, int whence) noexcept {
    auto* s = static_cast<State*>(opaque);
    if (whence == AVSEEK_SIZE) return s->encoded.count;
    whence &= ~AVSEEK_FORCE;
    const i64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? static_cast<i64>(s->cursor) :
        whence == SEEK_END ? static_cast<i64>(s->encoded.count) : -1;
    if (base < 0 || offset < -base || offset > static_cast<i64>(s->encoded.count) - base) return AVERROR(EINVAL);
    s->cursor = static_cast<u64>(base + offset);
    return static_cast<i64>(s->cursor);
}
AVPixelFormat hardware_only(AVCodecContext*, const AVPixelFormat* formats) noexcept {
    for (; *formats != AV_PIX_FMT_NONE; ++formats) {
        if (*formats == AV_PIX_FMT_D3D11) return *formats;
    }
    return AV_PIX_FMT_NONE; // Never silently time a software decode/upload.
}
} // namespace

bool mp4_ffmpeg_gpu_initialize(Mp4FfmpegGpuReference* r, ConstSlice<u8> encoded,
    ID3D11Device* device) noexcept {
    if (!r || !encoded.data || !encoded.count || !device) return false;
    mp4_ffmpeg_gpu_release(r);
    r->status = AVERROR(ENOMEM);
    if (!load_api()) { r->status = HRESULT_FROM_WIN32(load_error); return false; }
    auto* s = static_cast<State*>(VirtualAlloc(nullptr, sizeof(State), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!s) return false;
    r->state = s;
    s->encoded = encoded;
    s->previous_pts = AV_NOPTS_VALUE;
    s->format = api.avformat_alloc_context();
    auto* buffer = static_cast<u8*>(api.av_malloc(32768));
    if (!buffer) return false;
    s->io = api.avio_alloc_context(buffer, 32768, 0, s, read_bytes, nullptr, seek_bytes);
    if (!s->io) { api.av_free(buffer); return false; }
    if (!s->format) return false;
    s->format->pb = s->io;
    s->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    r->status = api.avformat_open_input(&s->format, nullptr, nullptr, nullptr);
    if (r->status < 0) return false;
    r->status = api.avformat_find_stream_info(s->format, nullptr);
    if (r->status < 0) return false;
    s->stream = api.av_find_best_stream(s->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (s->stream < 0) { r->status = s->stream; return false; }
    const AVCodecParameters* parameters = s->format->streams[s->stream]->codecpar;
    if (parameters->codec_id != AV_CODEC_ID_H264) { r->status = AVERROR(EINVAL); return false; }
    const AVCodec* codec = api.avcodec_find_decoder(parameters->codec_id);
    if (!codec) { r->status = AVERROR_DECODER_NOT_FOUND; return false; }
    s->codec = api.avcodec_alloc_context3(codec);
    s->packet = api.av_packet_alloc();
    s->frame = api.av_frame_alloc();
    s->device = api.av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!s->codec || !s->packet || !s->frame || !s->device) { r->status = AVERROR(ENOMEM); return false; }
    auto* hardware = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(s->device->data)->hwctx);
    // FFmpeg's device context takes ownership of every supplied COM reference.
    device->AddRef();
    hardware->device = device;
    r->status = api.av_hwdevice_ctx_init(s->device);
    if (r->status < 0) return false;
    r->status = api.avcodec_parameters_to_context(s->codec, parameters);
    if (r->status < 0) return false;
    s->codec->hw_device_ctx = api.av_buffer_ref(s->device);
    if (!s->codec->hw_device_ctx) { r->status = AVERROR(ENOMEM); return false; }
    s->codec->get_format = hardware_only;
    s->codec->pkt_timebase = s->format->streams[s->stream]->time_base;
    // Surface offsets are interpreted explicitly below; hardware frame pointers
    // must not be adjusted as if they were addressable CPU pixel buffers.
    s->codec->apply_cropping = 0;
    r->status = api.avcodec_open2(s->codec, codec, nullptr);
    return r->status >= 0;
}

Mp4DecodedFrame mp4_ffmpeg_gpu_read(Mp4FfmpegGpuReference* r, bool loop) noexcept {
    Mp4DecodedFrame result{};
    result.error = Mp4DecodeError::sample;
    auto* s = r ? static_cast<State*>(r->state) : nullptr;
    if (!s || !s->codec || !s->frame) return result;
    api.av_frame_unref(s->frame);
    r->frame = {};
    for (u32 attempt = 0; attempt < 8192; ++attempt) {
        r->status = api.avcodec_receive_frame(s->codec, s->frame);
        if (r->status == 0) {
            const AVFrame& frame = *s->frame;
            const auto* pool = frame.hw_frames_ctx ? reinterpret_cast<const AVHWFramesContext*>(frame.hw_frames_ctx->data) : nullptr;
            if (frame.format != AV_PIX_FMT_D3D11 || !pool || pool->sw_format != AV_PIX_FMT_NV12 ||
                !frame.data[0] || frame.width <= 0 || frame.height <= 0 ||
                frame.crop_left >= static_cast<u32>(frame.width) || frame.crop_right >= static_cast<u32>(frame.width) - frame.crop_left ||
                frame.crop_top >= static_cast<u32>(frame.height) || frame.crop_bottom >= static_cast<u32>(frame.height) - frame.crop_top ||
                ((frame.crop_left | frame.crop_top) & 1u) || reinterpret_cast<uintptr_t>(frame.data[1]) > ~0u) {
                r->status = AVERROR_INVALIDDATA; return result;
            }
            const u32 width = static_cast<u32>(frame.width - frame.crop_left - frame.crop_right);
            const u32 height = static_cast<u32>(frame.height - frame.crop_top - frame.crop_bottom);
            if (width > kMp4DecodeMaximumDimension || height > kMp4DecodeMaximumDimension ||
                (frame.pts != AV_NOPTS_VALUE && s->previous_pts != AV_NOPTS_VALUE && frame.pts < s->previous_pts)) {
                r->status = AVERROR_INVALIDDATA; return result;
            }
            s->previous_pts = frame.pts;
            r->frame.texture = reinterpret_cast<ID3D11Texture2D*>(frame.data[0]);
            r->frame.subresource = static_cast<u32>(reinterpret_cast<uintptr_t>(frame.data[1]));
            r->frame.crop_x = static_cast<u32>(frame.crop_left);
            r->frame.crop_y = static_cast<u32>(frame.crop_top);
            r->frame.width = width; r->frame.height = height;
            r->frame.bt709 = frame.colorspace == AVCOL_SPC_BT709;
            r->frame.full_range = frame.color_range == AVCOL_RANGE_JPEG;
            result.width = width; result.height = height; result.gpu = &r->frame;
            if (frame.duration > 0) result.duration_us = static_cast<u64>(api.av_rescale_q(frame.duration,
                s->format->streams[s->stream]->time_base, AVRational{1, 1000000}));
            result.error = Mp4DecodeError::none;
            return result;
        }
        if (r->status == AVERROR_EOF) {
            if (!loop) { result.error = Mp4DecodeError::end_of_stream; return result; }
            r->status = api.av_seek_frame(s->format, s->stream, 0, AVSEEK_FLAG_BACKWARD);
            if (r->status < 0) return result;
            api.avcodec_flush_buffers(s->codec);
            s->draining = false; s->previous_pts = AV_NOPTS_VALUE; loop = false;
            continue;
        }
        if (r->status != AVERROR(EAGAIN) || s->draining) return result;
        do {
            api.av_packet_unref(s->packet);
            r->status = api.av_read_frame(s->format, s->packet);
        } while (r->status >= 0 && s->packet->stream_index != s->stream);
        if (r->status == AVERROR_EOF) {
            s->draining = true;
            r->status = api.avcodec_send_packet(s->codec, nullptr);
        } else if (r->status >= 0) {
            r->status = api.avcodec_send_packet(s->codec, s->packet);
        }
        api.av_packet_unref(s->packet);
        // receive(EAGAIN) followed by send(EAGAIN) violates the API contract.
        // Do not drop a packet, manufacture a frame, or spin on decode failure.
        if (r->status < 0) return result;
    }
    r->status = AVERROR_INVALIDDATA;
    return result;
}

void mp4_ffmpeg_gpu_release(Mp4FfmpegGpuReference* r) noexcept {
    if (!r) return;
    auto* s = static_cast<State*>(r->state);
    if (s) {
        api.av_frame_free(&s->frame);
        api.av_packet_free(&s->packet);
        api.avcodec_free_context(&s->codec);
        api.av_buffer_unref(&s->device);
        api.avformat_close_input(&s->format);
        // libavformat may replace AVIO's allocation during probing.
        if (s->io) api.av_free(s->io->buffer);
        api.avio_context_free(&s->io);
        VirtualFree(s, 0, MEM_RELEASE);
    }
    *r = {};
}
} // namespace maicord
#else
namespace maicord {
bool mp4_ffmpeg_gpu_initialize(Mp4FfmpegGpuReference* r, ConstSlice<u8>, ID3D11Device*) noexcept {
    if (r) r->status = -1;
    return false;
}
Mp4DecodedFrame mp4_ffmpeg_gpu_read(Mp4FfmpegGpuReference*, bool) noexcept {
    Mp4DecodedFrame result{};
    result.error = Mp4DecodeError::runtime;
    return result;
}
void mp4_ffmpeg_gpu_release(Mp4FfmpegGpuReference*) noexcept {}
} // namespace maicord
#endif
