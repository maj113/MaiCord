#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/media/gpu_video_frame.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/sys/d3d11.hpp"

struct ID3D11Device;

namespace maicord {

// Live H.264 (RTP camera/screen share) DXVA decoder, one per remote video
// stream. Input is one depacketized Annex B frame per push; parameter sets may
// change mid-stream and an IDR with new geometry or DPB size recreates the
// DXVA decoder. Loss (frame_num gaps, damaged or missing slices, GPU
// rejection) freezes on the last good frame and asks for a keyframe. No CPU
// decode fallback and no MP4 state; attachments keep h264_gpu_decoder.
//
// Each output picture is copied on the shared multithread-protected immediate
// context into a decoder-owned NV12 texture (subresource 0, BIND_SHADER_RESOURCE)
// before the sink sees it, so presentation may read it at any later time
// without a lease: GPU command order keeps every enqueued read consistent.
// The texture pointer changes only when the stream geometry changes; a sink
// that keeps it beyond the callback must AddRef the texture and both optional
// plane views. A failed view pair leaves both pointers null for the copy path.
struct H264StreamFrame {
    const GpuVideoFrame* gpu; // Valid during the callback; copy the struct to keep it.
    u32 rtp_timestamp;
    u16 display_width, display_height; // Sample-aspect-corrected layout extent.
    bool keyframe;
};
// Callbacks run on the decoder thread (or inside push for a synchronous
// decoder) and must not call back into the same decoder.
struct H264StreamSink {
    void* context;
    void (*frame)(void* context, u32 ssrc, const H264StreamFrame& frame) noexcept;
    // Throttled to one request per kH264StreamKeyframeIntervalMs while waiting.
    void (*request_keyframe)(void* context, u32 ssrc) noexcept;
};
inline constexpr u32 kH264StreamKeyframeIntervalMs = 500;
// Frames queued for the decode thread. Overflow drops the queue and waits for
// an IDR rather than accumulating latency.
inline constexpr u32 kH264StreamQueueFrames = 8;
inline constexpr u32 kH264StreamMaximumFrameBytes = 16u * 1024u * 1024u;
// A stream that receives no input for this long (its tile is hidden, the
// call view closed or minimized, so the video is no longer wanted) releases
// its DXVA decoder, surface pool and display copy. The next picture asks for
// a keyframe and the IDR recreates them (one 1080p stream: ~10-20 MB).
inline constexpr u32 kH264StreamIdleReleaseMs = 5'000;

struct H264StreamDecoderOptions {
    ID3D11Device* device; // Multithread-protected; its immediate context is shared.
    u32 ssrc;
    H264StreamSink sink;
    bool synchronous; // Decode inside push (tests/tools); no thread is created.
};
struct H264StreamDecoderStats {
    u32 received, decoded, presented, dropped, damaged, gaps, late;
    u32 keyframe_requests, reinitializations, gpu_failures, overflows;
    u32 coded_width, coded_height, surfaces;
    u32 idle_releases;
    i32 last_status; // Last failing HRESULT, S_OK when none.
    bool unsupported; // The stream (or the GPU) cannot decode the current SPS.
    bool device_lost;
};
struct H264StreamDecoder;

// Creates the queue and thread only; DXVA objects are created at the first
// decodable IDR and recreated when an IDR changes geometry/profile limits.
H264StreamDecoder* h264_stream_decoder_create(const H264StreamDecoderOptions& options) noexcept;
// Copies the frame; never waits for the GPU. False when the frame was dropped.
// intact=false (the RTP assembler lost packets of this access unit) still
// applies its parameter sets but never submits its slices: a partial
// reference picture would corrupt every frame until the next IDR.
bool h264_stream_decoder_push(H264StreamDecoder* decoder, ConstSlice<u8> annexb, u32 rtp_timestamp,
                              bool intact = true) noexcept;
// Releases the DXVA objects of an input-idle stream (the decode thread does
// this itself after kH264StreamIdleReleaseMs). False when nothing was held.
// Synchronous decoders only: threaded decoders own their GPU objects.
bool h264_stream_decoder_release_idle(H264StreamDecoder* decoder) noexcept;
H264StreamDecoderStats h264_stream_decoder_stats(const H264StreamDecoder* decoder) noexcept;
// Joins the decode thread, then releases every GPU object and buffer.
void h264_stream_decoder_destroy(H264StreamDecoder* decoder) noexcept;

} // namespace maicord
