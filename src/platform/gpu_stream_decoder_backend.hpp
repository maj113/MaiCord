#pragma once

#include "maicord/platform/gpu_stream_decoder.hpp"
#include "maicord/media/h264_stream.hpp"
#include "maicord/media/hevc_stream.hpp"
#include "gpu_decoder_surfaces.hpp"
#include "av1_gpu_stream.hpp"

namespace maicord {

// Shared state of a live stream decoder and the codec backends that drive it.
// Backends run on the decode thread only.
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
struct GpuStream {
    ID3D11Device* device;
    ID3D11DeviceContext* immediate;
    ID3D11VideoDevice* video;
    ID3D11VideoContext* context;
    ID3D11Multithread* guard;
    ID3D11VideoDecoder* decoder;
    GpuDecoderSurfaces surfaces;
    GpuVideoFrame frame; // Format of the current geometry; texture and slice set per picture.
    u32 coded_width, coded_height, feedback;
    u16 display_width, display_height;
    u16 slice_entries;  // Slice control entries per submission, 0 until queried.
    u8 presented;       // Slice of the newest presented picture, 255 when none.
    volatile LONG hold; // Reader-held slice + 1, 0 when none.
    GpuStreamDecoderOptions options;
    GpuStreamDecoderStats stats;
    u64 last_request_ms;
    SRWLOCK lock;
    HANDLE wake, thread;
    GpuStreamInput queue[kGpuStreamQueueFrames];
    GpuStreamInput work;
    u32 head, count;
    bool stop, resynchronize_pending;
    union {
        H264GpuStreamState h264;
        HevcGpuStreamState hevc;
        Av1GpuStreamState av1;
    };
};

struct GpuStreamDecoderDesc {
    const GUID* profile;
    u32 width, height;                 // Coded size.
    u32 surface_width, surface_height; // Allocation size (codec alignment).
    DXGI_RATIONAL rate;
    u8 bitstream_raw; // Short-slice ConfigBitstreamRaw value of the profile.
    u8 surfaces;
};
// Recreates the DXVA decoder and its shader-readable surface array.
// DXGI_ERROR_UNSUPPORTED when the GPU cannot decode this geometry.
HRESULT gpu_stream_create_decoder(GpuStream* stream, const GpuStreamDecoderDesc& desc) noexcept;
// Writes the bitstream and control entries of units [first, first + n) for
// the largest n <= limit that fits capacity; returns n (0: unit first does
// not fit) and the bitstream bytes.
using GpuStreamBatchFill = u32 (*)(const void* source, u32 first, u8* bitstream, u32 capacity,
                                   void* entries, u32 limit, u32* bytes) noexcept;
struct GpuStreamSubmission {
    u8 surface;
    ConstSlice<u8> parameters, matrix; // matrix may be empty.
    u32 units;                         // Slices or tiles.
    u32 entry_bytes, entry_limit;      // Control entry size and the most per batch.
    const void* source;
    GpuStreamBatchFill fill;
};
// One picture into surface in one Begin/EndFrame: the parameters (and matrix)
// with each batch of units that fits the driver's bitstream and control buffers.
HRESULT gpu_stream_submit(GpuStream* stream, const GpuStreamSubmission& submission) noexcept;
// H.264/HEVC short slices (one entry layout) from Annex B NAL ranges of source.
HRESULT gpu_stream_submit_slices(GpuStream* stream, u8 surface, ConstSlice<u8> parameters,
                                 ConstSlice<u8> matrix, ConstSlice<u8> source,
                                 const H264StreamNalRange* slices, u32 slice_count) noexcept;
// Display format at a keyframe: crop and visible size in coded pixels.
void gpu_stream_set_format(GpuStream* stream, u32 crop_x, u32 crop_y, u32 width, u32 height, u8 matrix,
                           bool full_range, u32 sar_width, u32 sar_height) noexcept;
// Publishes the decoded picture in surface to the sink.
void gpu_stream_present(GpuStream* stream, u8 surface, u32 rtp_timestamp, bool keyframe) noexcept;
// Slices a new picture must not be decoded into (presented and held).
u32 gpu_stream_held(const GpuStream& stream) noexcept;
void gpu_stream_request_keyframe(GpuStream* stream, bool unsupported) noexcept;
// Records a failed GPU call; true when the device is gone.
bool gpu_stream_failure(GpuStream* stream, HRESULT status) noexcept;
void gpu_stream_release_decoder(GpuStream* stream) noexcept;

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

} // namespace maicord
