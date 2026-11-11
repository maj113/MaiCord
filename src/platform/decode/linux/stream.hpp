#pragma once

#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/media/video/h264_stream.hpp"
#include "maicord/media/video/hevc_stream.hpp"
#include "maicord/platform/decode/gpu_stream_decoder.hpp"
#include "../av1_gpu_stream.hpp"

namespace maicord {

// The Linux live stream decoder (gpu_stream_decoder.hpp): the queue, the decode thread, the picture surfaces and the
// hold rules that gpu_stream_decoder.cpp has for Direct3D 11 (whose backend header, gpu_stream_decoder_backend.hpp,
// is the DXVA one), over two tiers of decoder:
//
//   hardware  VA-API (src/platform/decode/vaapi): the in-house parsers of src/media/video build the VA parameter
//             and slice buffers, the decoder writes into VASurfaces that are exported as dma-bufs and imported as
//             EGLImages, so a picture is two zero-copy planes
//   software  libopenh264 and libdav1d (src/platform/decode/software): pictures in CPU memory, copied into planes
//             the renderer uploads
//
// Every backend runs on the decode thread only and never calls GL: the planes it publishes are made without GL
// (maicord/gpu/video_planes.hpp) and realized by the renderer's thread.

struct GpuStream;

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

struct StreamBackend {
    // True for the hardware tier: its `unsupported` verdict falls back to the software tier.
    bool hardware;
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
// The software tier's state: one library decoder and the stream's wait for a keyframe.
struct SoftwareDecoder;
struct SoftwareStreamState {
    SoftwareDecoder* decoder;
    u32 decoded, dropped, damaged, gaps;
    u32 next_surface;
    bool waiting, unsupported;
};

struct GpuStream {
    GpuDevice* device; // referenced
    const StreamBackend* backend;
    void* session; // the backend's decoder object (a VaSession for the hardware tier)
    StreamSurfaces surfaces;
    GpuVideoFrame frame; // Format of the current geometry; planes set per picture.
    u32 coded_width, coded_height;
    u16 display_width, display_height;
    u8 presented;       // Slot of the newest presented picture, 255 when none.
    u8 hardware_failures; // Pictures the GPU decoder failed in a row; the tier is given up after a few
    volatile LONG hold; // Reader-held slot + 1, 0 when none.
    GpuStreamDecoderOptions options;
    // The decode thread's counters (the pushing thread's for a synchronous stream); `received` is in `published`.
    GpuStreamDecoderStats stats;
    // What gpu_stream_decoder_stats returns, under `lock`: the pusher counts `received` here, and publish_stats copies the
    // rest of `stats` after every decode.
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
        SoftwareStreamState software;
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
// Frees a slot's plane handles (the memory of a CPU slot is the planes').
void stream_slot_release(StreamSurface* slot) noexcept;
// Frees every slot's plane handles and memory.
void stream_surfaces_release(GpuStream* stream) noexcept;
// Replaces the planes of `slot` with the export of a VA surface (vaapi::export_picture): the old planes go once the
// new ones are in, and false leaves the slot as it was.
namespace vaapi {
struct Session;
}
bool stream_slot_export(GpuDevice* device, vaapi::Session* session, u32 surface, StreamSurface* slot) noexcept;
// CPU-backed slots (the software tier): `count` pictures of coded_width x coded_height (both even).
bool stream_surfaces_create_cpu(GpuStream* stream, u32 count, u32 coded_width, u32 coded_height) noexcept;
// Copies a decoded software picture into the CPU planes of slot and publishes them; the bytes written. The slot is at
// least as large as the picture (both planes, chroma interleaved).
struct SoftwarePicture;
u64 stream_copy_picture(const StreamSurface& slot, const SoftwarePicture& picture) noexcept;
// The movie decoders' (h264_file_decoder.cpp, hevc_file_decoder.cpp) picture slots: frees the planes of the first
// *count slots, then forgets them all.
void stream_slots_release(StreamSurface (&slots)[kStreamSurfaceLimit], u32* count) noexcept;
// Makes the first `wanted` slots CPU-backed pictures of width x height (both even), *count of them; false (every slot
// freed) when one could not be made.
bool stream_slots_create_cpu(GpuDevice* device, StreamSurface (&slots)[kStreamSurfaceLimit], u32* count, u32 wanted,
                             u32 width, u32 height) noexcept;
// The movie picture the output queue shows next, as *frame's planes: a hardware picture (session) is exported into
// its slot now (drivers differ in when a surface's dma-buf is complete, and a fresh export is always right).
struct H264OutputQueue;
struct Mp4DecodedFrame;
struct VideoDisplayExtent;
Mp4DecodedFrame stream_movie_frame(H264OutputQueue* output, GpuDevice* device, vaapi::Session* session,
                                   StreamSurface* slots, GpuVideoFrame* frame,
                                   const VideoDisplayExtent& display) noexcept;

// ---- Backends ----------------------------------------------------------------------------------------------------

extern const StreamBackend kSoftwareStreamBackend;
// The hardware backend of a codec (VA-API), null when this build or machine has none.
const StreamBackend* hardware_stream_backend(GpuStreamCodec codec) noexcept;
// The codecs the VA-API decoders can decode on the GPU `device` renders on (a gpu_stream_codec_bit set); the first
// call probes, and the answer is kept.
u32 hardware_probe_codecs(GpuDevice* device) noexcept;
// True when a keyframe is in the unit (H.264 IDR, AV1 sequence header): where a stream can start.
bool stream_unit_is_keyframe(GpuStreamCodec codec, ConstSlice<u8> bytes) noexcept;

} // namespace maicord
