#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/media/gpu_video_frame.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/sys/d3d11.hpp"

struct ID3D11Device;

namespace maicord {

// Live (RTP camera/screen share) DXVA decoder, one per remote video stream.
// Input is one depacketized Annex B access unit (AV1: temporal unit of OBUs,
// the last without obu_size) per push; parameter sets may
// change mid-stream and a keyframe with new geometry or a larger DPB recreates
// the DXVA decoder. Loss (reference gaps, damaged or missing slices, GPU
// rejection) freezes on the last good picture and asks for a keyframe. No CPU
// decode fallback; attachments keep their own decoders.
//
// Output is zero-copy: a picture is one slice of the decoder's shader-readable
// NV12 array (gpu_decoder_surfaces) with that slice's plane views. The slice
// of the newest presented picture and the slice named by the last
// gpu_stream_decoder_hold are never decoded into, so a reader that enqueues
// its conversion on the shared multithread-protected immediate context before
// it holds a newer picture always reads complete pixels. The texture changes
// only when the stream's geometry does; a sink that keeps the frame beyond
// the callback must AddRef the texture and both plane views.
enum class GpuStreamCodec : u8 {
    h264,
    h265,
    av1
};
constexpr u32 gpu_stream_codec_bit(GpuStreamCodec codec) noexcept {
    return 1u << static_cast<u8>(codec);
}
struct GpuStreamFrame {
    const GpuVideoFrame* gpu; // Valid during the callback; copy the struct to keep it.
    u32 rtp_timestamp;
    u16 display_width, display_height; // Sample-aspect-corrected layout extent.
    bool keyframe;
};
// Callbacks run on the decoder thread (or inside push for a synchronous
// decoder) and must not call back into the same decoder except hold.
struct GpuStreamSink {
    void* context;
    void (*frame)(void* context, u32 ssrc, const GpuStreamFrame& frame) noexcept;
    // Throttled to one request per kGpuStreamKeyframeIntervalMs while waiting.
    void (*request_keyframe)(void* context, u32 ssrc) noexcept;
};
inline constexpr u32 kGpuStreamKeyframeIntervalMs = 500;
// Frames queued for the decode thread. Overflow drops the queue and waits for
// a keyframe rather than accumulating latency.
inline constexpr u32 kGpuStreamQueueFrames = 8;
inline constexpr u32 kGpuStreamMaximumFrameBytes = 16u * 1024u * 1024u;
// A stream that receives no input for this long (its tile is hidden, the
// call view closed or minimized) releases its DXVA decoder and surfaces. The
// next picture asks for a keyframe, which recreates them.
inline constexpr u32 kGpuStreamIdleReleaseMs = 5'000;

struct GpuStreamDecoderOptions {
    ID3D11Device* device; // Multithread-protected; its immediate context is shared.
    u32 ssrc;
    GpuStreamSink sink;
    GpuStreamCodec codec;
    bool synchronous; // Decode inside push (tests/tools); no thread is created.
};
struct GpuStreamDecoderStats {
    u32 received, decoded, presented, dropped, damaged, gaps, late;
    u32 keyframe_requests, reinitializations, gpu_failures, overflows;
    u32 coded_width, coded_height, surfaces;
    u32 idle_releases;
    u64 uploaded_bytes; // Picture parameters, matrices, bitstream and slice control sent to the driver.
    i32 last_status;    // Last failing HRESULT, S_OK when none.
    bool unsupported;   // The stream (or the GPU) cannot decode the current parameter sets.
    bool device_lost;
};
struct GpuStreamDecoder;

// gpu_stream_codec_bit set of the codecs the device decodes in hardware with
// the DXVA bitstream format used here (short slices; AV1 tiles) and
// shader-readable NV12 output (what create can use).
u32 gpu_stream_decoder_codecs(ID3D11Device* device) noexcept;
// Creates the queue and thread only; DXVA objects are created at the first
// decodable keyframe and recreated when a keyframe changes geometry or DPB needs.
GpuStreamDecoder* gpu_stream_decoder_create(const GpuStreamDecoderOptions& options) noexcept;
// Copies the frame; never waits for the GPU. False when the frame was dropped.
// intact=false (the RTP assembler lost packets this access unit depends on)
// still applies its parameter sets but never submits its slices.
bool gpu_stream_decoder_push(GpuStreamDecoder* decoder, ConstSlice<u8> annexb, u32 rtp_timestamp,
                             bool intact = true) noexcept;
// Any thread: a reader took the presented picture in this slice and will
// enqueue its read before holding another one. Replaces the previous hold.
void gpu_stream_decoder_hold(GpuStreamDecoder* decoder, u32 subresource) noexcept;
// Releases the DXVA objects of an input-idle stream (the decode thread does
// this itself after kGpuStreamIdleReleaseMs). False when nothing was held.
// Synchronous decoders only: threaded decoders own their GPU objects.
bool gpu_stream_decoder_release_idle(GpuStreamDecoder* decoder) noexcept;
GpuStreamDecoderStats gpu_stream_decoder_stats(const GpuStreamDecoder* decoder) noexcept;
// Joins the decode thread, then releases every GPU object and buffer.
void gpu_stream_decoder_destroy(GpuStreamDecoder* decoder) noexcept;

} // namespace maicord
