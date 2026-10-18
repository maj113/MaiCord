#pragma once
#include "maicord/app/video_frame_sink.hpp"
#include "maicord/media/gpu_video_frame.hpp"

#include "maicord/core/win32_records.hpp"
#include "maicord/sys/d3d11.hpp"

struct ID3D11Device;

namespace maicord {

struct H264StreamDecoder;

// Decoder side of VideoFrameSink: one H.264 DXVA stream decoder (with its own
// decode thread) per remote video SSRC, created on the stream's first keyframe
// and destroyed by end_stream or release. Each stream keeps a "latest frame":
// a decoder-owned NV12 texture rewritten on the shared immediate context, so
// the renderer may present it at any time without leases.
inline constexpr u32 kVideoStreamDecoderStreams = 16;

// Crosses the account-module ABI. Each non-null texture or plane view carries
// one reference owned by the caller. Release all after the copy/draw; pixels are the
// newest decoded picture at the time the GPU executes the read.
struct VideoStreamFrame {
    GpuVideoFrame gpu;
    u32 ssrc, rtp_timestamp;
    u32 sequence; // Pictures presented on this stream; changes when the content does.
    u16 display_width, display_height; // Aspect-corrected layout extent.
    bool keyframe;
    u8 reserved[7];
};
static_assert(sizeof(VideoStreamFrame) == 72);

struct VideoStreamDecoders {
    struct Stream {
        H264StreamDecoder* decoder;
        GpuVideoFrame latest; // Owns each non-null texture and plane view.
        u32 ssrc, rtp_timestamp, sequence;
        u16 display_width, display_height;
        bool keyframe;
    };
    SrwLock lock;
    ID3D11Device* device; // Referenced; null until the renderer provides one.
    VideoKeyframeRequester keyframes;
    Stream streams[kVideoStreamDecoderStreams];
    // Worker-only: SSRCs waiting for their first keyframe, for request throttling.
    u32 waiting_ssrc[kVideoStreamDecoderStreams];
    u64 waiting_ms[kVideoStreamDecoderStreams];
};

void video_stream_decoders_initialize(VideoStreamDecoders* decoders) noexcept;
// Any thread. The renderer's multithread-protected device; streams created
// later decode on it (existing streams keep theirs until they end).
void video_stream_decoders_set_device(VideoStreamDecoders* decoders, ID3D11Device* device) noexcept;
// Any thread. Decode threads forward keyframe requests here; {} disables.
void video_stream_decoders_set_requester(VideoStreamDecoders* decoders, VideoKeyframeRequester requester) noexcept;
// push_frame/end_stream run on the voice worker.
VideoFrameSink video_stream_decoders_sink(VideoStreamDecoders* decoders) noexcept;
// Any thread (UI). False when the SSRC has no decoded picture yet.
bool video_stream_decoders_frame(VideoStreamDecoders* decoders, u32 ssrc, VideoStreamFrame* output) noexcept;
// Joins every decode thread and releases all GPU references.
void video_stream_decoders_release(VideoStreamDecoders* decoders) noexcept;

} // namespace maicord
