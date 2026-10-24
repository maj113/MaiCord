#pragma once
#include "maicord/app/video_frame_sink.hpp"
#include "maicord/media/gpu_video_frame.hpp"

#include "maicord/core/win32_records.hpp"

struct ID3D11Device;

namespace maicord {

struct GpuStreamDecoder;

// Decoder side of VideoFrameSink: one DXVA stream decoder (with its own decode
// thread) per remote video SSRC, created on the stream's first keyframe in a
// codec the device decodes, replaced when the stream's codec changes, and
// destroyed by end_stream or release. Each stream keeps its newest picture: a
// slice of the decoder's own surface array (zero-copy), which the decoder
// never overwrites while it is the newest or the slice last handed out by
// video_stream_decoders_frame.
inline constexpr u32 kVideoStreamDecoderStreams = 16;

// Crosses the account-module ABI. Each non-null texture or plane view carries
// one reference owned by the caller. Enqueue the read before asking for this
// stream's next frame, then release all references.
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
        GpuStreamDecoder* decoder;
        GpuVideoFrame latest; // Owns each non-null texture and plane view.
        u32 ssrc, rtp_timestamp, sequence;
        u16 display_width, display_height;
        VoiceVideoCodec codec;
        bool keyframe;
    };
    SrwLock lock;
    ID3D11Device* device; // Referenced; null until the renderer provides one.
    VideoKeyframeRequester keyframes;
    volatile LONG decodable; // voice_video_codec_bit set of the device's hardware decoders.
    Stream streams[kVideoStreamDecoderStreams];
    // Worker-only: SSRCs waiting for their first keyframe, for request throttling.
    u32 waiting_ssrc[kVideoStreamDecoderStreams];
    u64 waiting_ms[kVideoStreamDecoderStreams];
};

void video_stream_decoders_initialize(VideoStreamDecoders* decoders) noexcept;
// Any thread. The renderer's multithread-protected device; probes its decoders.
// Streams created later decode on it (existing streams keep theirs until they end).
void video_stream_decoders_set_device(VideoStreamDecoders* decoders, ID3D11Device* device) noexcept;
// Any thread. Decode threads forward keyframe requests here; {} disables.
void video_stream_decoders_set_requester(VideoStreamDecoders* decoders, VideoKeyframeRequester requester) noexcept;
// push_frame/end_stream run on the voice worker; decodable on any thread.
VideoFrameSink video_stream_decoders_sink(VideoStreamDecoders* decoders) noexcept;
// Any thread (UI). False when the SSRC has no decoded picture yet. The
// returned slice stays intact until this stream's next call.
bool video_stream_decoders_frame(VideoStreamDecoders* decoders, u32 ssrc, VideoStreamFrame* output) noexcept;
// Joins every decode thread and releases all GPU references.
void video_stream_decoders_release(VideoStreamDecoders* decoders) noexcept;

} // namespace maicord
