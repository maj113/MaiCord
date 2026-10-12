#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// Boundary between call-video transport and a decoder. The transport side
// (voice_video_sink) delivers complete access units after RTP assembly and DAVE:
// Annex B with 4-byte start codes, in RTP order, one stream per media SSRC.
// The decoder side asks for keyframes when it lacks a decodable reference.
struct VideoFrameSink {
    void* context;
    // Runs on the voice worker. The frame is borrowed for the call only; the
    // decoder copies what it keeps and must not block. False means dropped.
    // intact is false when the RTP core lost data the frame depends on (a
    // reference since the last keyframe): the decoder applies its parameter
    // sets but freezes on the last good picture and asks for a keyframe.
    bool (*push_frame)(void* context, u32 ssrc, ConstSlice<u8> frame, u32 rtp_timestamp, bool keyframe,
                       bool intact) noexcept;
    // Optional: the SSRC stopped carrying this stream (sender left or was
    // remapped, call ended). Release per-stream decoder state.
    void (*end_stream)(void* context, u32 ssrc) noexcept;
};
// Decoder -> transport: request a keyframe (RTCP PLI) for a media SSRC. Safe to
// call from any thread (e.g. a decode thread); the worker sends it on its next
// drive. Matches H264StreamSink::request_keyframe.
using VideoKeyframeRequest = void (*)(void* context, u32 ssrc) noexcept;
struct VideoKeyframeRequester {
    void* context;
    VideoKeyframeRequest request_keyframe;
};
} // namespace maicord
