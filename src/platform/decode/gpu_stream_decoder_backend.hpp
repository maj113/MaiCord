#pragma once

#include "maicord/platform/decode/gpu_stream_decoder.hpp"
#include "maicord/media/video/h264_stream.hpp"
#include "maicord/media/video/hevc_stream.hpp"
#include "maicord/os/thread.hpp"
#include "av1_gpu_stream.hpp"
#if defined(_WIN32)
#include "gpu_decoder_surfaces.hpp"
#endif

namespace maicord {

// Shared state of a live stream decoder (gpu_stream_decoder.cpp: the queue, the decode thread, the picture slots and
// the hold rules) and the codec backends that drive it, on the decode thread only: DXVA on Windows, the VA-API and
// software tiers of linux/stream.hpp on Linux.
struct GpuStream;

struct StreamBackend {
#if !defined(_WIN32)
    // True for the hardware tier: its `unsupported` verdict falls back to the software tier.
    bool hardware;
#endif
    void (*reset)(GpuStream* stream) noexcept;
    void (*decode)(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact,
                   bool resynchronize) noexcept;
    void (*drain)(GpuStream* stream) noexcept;
    // Frees the decoder and its surfaces (an idle stream, a tier switch, destruction).
    void (*release)(GpuStream* stream) noexcept;
    void (*stats)(const GpuStream& stream, GpuStreamDecoderStats* stats) noexcept;
};

struct GpuStreamInput {
    u8* data;
    u32 bytes, capacity, rtp_timestamp;
    bool intact, resynchronize;
};
struct H264GpuStreamState {
    H264StreamState state;
    H264StreamPicture picture;
};
struct HevcGpuStreamState {
    HevcStreamState state;
    HevcStreamPicture picture;
};
#if !defined(_WIN32)
// One picture slot: the two planes of a picture (the luma texture is the frame's texture) and, for the software
// tier, the CPU memory behind them.
struct StreamSurface {
    GpuTexture* luma_texture;
    GpuSrv* luma_view;
    GpuTexture* chroma_texture;
    GpuSrv* chroma_view;
    u8* luma_pixels;
    u8* chroma_pixels;
    u32 luma_pitch, chroma_pitch;
};
inline constexpr u32 kStreamSurfaceLimit = 32;
struct StreamSurfaces {
    StreamSurface slot[kStreamSurfaceLimit];
    u32 count;
    u32 width, height; // coded extent of every slot
};
// The software tier's state: one library decoder and the stream's wait for a keyframe.
struct SoftwareDecoder;
struct SoftwareStreamState {
    SoftwareDecoder* decoder;
    u32 decoded, dropped, damaged, gaps;
    u32 next_surface;
    bool waiting, unsupported;
};
#endif

struct GpuStream {
    GpuDevice* device; // referenced
    const StreamBackend* backend;
#if defined(_WIN32)
    ID3D11DeviceContext* immediate;
    ID3D11VideoDevice* video;
    ID3D11VideoContext* context;
    ID3D11Multithread* guard;
    ID3D11VideoDecoder* decoder;
    GpuDecoderSurfaces surfaces; // Made with the decoder and released with it.
#else
    void* session; // the backend's decoder object (a VaSession for the hardware tier)
    StreamSurfaces surfaces;
#endif
    GpuVideoFrame frame; // Format of the current geometry; planes set per picture.
    u32 coded_width, coded_height;
#if defined(_WIN32)
    u32 feedback;
#endif
    u16 display_width, display_height;
#if defined(_WIN32)
    u16 slice_entries; // Slice control entries per submission, 0 until queried.
#endif
    u8 presented; // Slot of the newest presented picture, 255 when none.
#if !defined(_WIN32)
    u8 hardware_failures; // Pictures the GPU decoder failed in a row; the tier is given up after a few
#endif
    volatile LONG hold; // Reader-held slot + 1, 0 when none.
    GpuStreamDecoderOptions options;
    // The decode thread's counters (the pushing thread's for a synchronous stream); `received` is in `published`.
    GpuStreamDecoderStats stats;
    // What gpu_stream_decoder_stats returns, under `lock`: the pusher counts `received` here, and publish_stats copies
    // the rest of `stats` after every decode.
    GpuStreamDecoderStats published;
    u64 last_request_ms;
    SRWLOCK lock;
    Waitable wake;
    Thread* thread;
    GpuStreamInput queue[kGpuStreamQueueFrames];
    GpuStreamInput work;
    u32 head, count;
    bool stop, resynchronize_pending;
    union {
        H264GpuStreamState h264;
        HevcGpuStreamState hevc;
        Av1GpuStreamState av1;
#if !defined(_WIN32)
        SoftwareStreamState software;
#endif
    };
};

// ---- What every backend calls ---------------------------------------------------------------------------------

// Display format at a keyframe: crop and visible size in coded pixels.
void stream_set_format(GpuStream* stream, u32 crop_x, u32 crop_y, u32 width, u32 height, u8 matrix, bool full_range,
                       u32 sar_width, u32 sar_height) noexcept;
// Publishes the decoded picture in slot to the sink.
void stream_present(GpuStream* stream, u8 slot, u32 rtp_timestamp, bool keyframe) noexcept;
// Slots a new picture must not be decoded into (presented and held), as a bit set.
u32 stream_held(const GpuStream& stream) noexcept;
void stream_request_keyframe(GpuStream* stream, bool unsupported) noexcept;
// The backend a new stream of `codec` on `device` decodes with; null when none can.
const StreamBackend* stream_backend(GpuDevice* device, GpuStreamCodec codec) noexcept;

// ---- The hardware codec backends, both systems (h264_gpu_stream_decoder.cpp, hevc_gpu_stream_decoder.cpp,
// av1_gpu_stream.cpp) ----------------------------------------------------------------------------------------------

void h264_gpu_stream_reset(GpuStream* stream) noexcept;
void h264_gpu_stream_decode(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact,
                            bool resynchronize) noexcept;
void h264_gpu_stream_drain(GpuStream* stream) noexcept;
void h264_gpu_stream_stats(const GpuStream& stream, GpuStreamDecoderStats* stats) noexcept;
void hevc_gpu_stream_reset(GpuStream* stream) noexcept;
void hevc_gpu_stream_decode(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact,
                            bool resynchronize) noexcept;
void hevc_gpu_stream_drain(GpuStream* stream) noexcept;
void hevc_gpu_stream_stats(const GpuStream& stream, GpuStreamDecoderStats* stats) noexcept;

// ---- Their system half: DXVA (gpu_stream_dxva.cpp), VA-API (vaapi/va_stream.cpp) ------------------------------
// A GPU call that fails is counted (stats.gpu_failures); a failure that gives the stream up on this GPU sets the
// codec state's `unsupported`.

inline bool stream_decoder_open(const GpuStream& stream) noexcept {
#if defined(_WIN32)
    return stream.decoder != nullptr;
#else
    return stream.session != nullptr;
#endif
}
// Shows the picture decoded into slot (stream_present); false when it could not be shown.
#if defined(_WIN32)
inline bool stream_show(GpuStream* stream, u8 slot, u32 rtp_timestamp, bool keyframe, bool*) noexcept {
    stream_present(stream, slot, rtp_timestamp, keyframe);
    return true;
}
#else
bool stream_show(GpuStream* stream, u8 slot, u32 rtp_timestamp, bool keyframe, bool* unsupported) noexcept;
#endif
// (Re)creates the decoder and `surfaces` surfaces for the SPS; false when the GPU cannot decode it or failed.
bool h264_gpu_stream_create(GpuStream* stream, const H264Sps& sps, u8 surfaces) noexcept;
bool hevc_gpu_stream_create(GpuStream* stream, const HevcSps& sps, u8 surfaces) noexcept;
// Decodes the picture's slices of `bytes` into decode.surface; false when a header or the GPU refused them.
bool h264_gpu_stream_submit(GpuStream* stream, const H264StreamPicture& picture, const H264StreamDecode& decode,
                            ConstSlice<u8> bytes) noexcept;
bool hevc_gpu_stream_submit(GpuStream* stream, const HevcStreamPicture& picture, const HevcStreamDecode& decode,
                            ConstSlice<u8> bytes) noexcept;

} // namespace maicord
