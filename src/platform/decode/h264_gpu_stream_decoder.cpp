#include "gpu_stream_decoder_backend.hpp"
// The H.264 backend of both systems over their submission (h264_gpu_stream_create, h264_gpu_stream_submit): once
// per frame and no byte loop, compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {

void present_ready(GpuStream* stream) noexcept {
    H264StreamState* state = &stream->h264.state;
    H264StreamOutput output{};
    while (h264_stream_pop(state, &output))
        if (!stream_show(stream, output.surface, output.rtp_timestamp, output.keyframe, &state->unsupported))
            h264_stream_reset(state);
}

} // namespace

void h264_gpu_stream_reset(GpuStream* stream) noexcept {
    h264_stream_reset(&stream->h264.state);
    h264_stream_set_surfaces(&stream->h264.state, 0);
}

void h264_gpu_stream_drain(GpuStream* stream) noexcept {
    stream->h264.state.drain = true;
    present_ready(stream);
}

void h264_gpu_stream_decode(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact,
                            bool resynchronize) noexcept {
    H264StreamState* state = &stream->h264.state;
    H264StreamPicture& picture = stream->h264.picture;
    if (resynchronize)
        h264_stream_lost(state);
#if defined(_WIN32)
    if (stream->stats.device_lost) // Only DXVA reports a lost device.
        return;
#endif
    auto scanner = h264_stream_scanner(bytes);
    for (;;) {
        const H264ScanStatus scanned = h264_stream_scan(&state->sets, &scanner, &picture);
        if (scanned == H264ScanStatus::end)
            break;
        if (scanned == H264ScanStatus::damaged || !intact) {
            ++state->damaged;
            if (state->active && !state->waiting)
                h264_stream_lost(state);
            stream_request_keyframe(stream, state->unsupported);
            continue;
        }
        state->held = stream_held(*stream);
        H264StreamDecode decode{};
        const H264StreamAction action = h264_stream_prepare(state, picture, &decode);
        if (action == H264StreamAction::unsupported)
            continue;
        if (action != H264StreamAction::decode) {
            stream_request_keyframe(stream, state->unsupported);
            continue;
        }
        present_ready(stream); // Drain before an IDR or a surface replacement.
        if (decode.reinitialize) {
            const bool created = h264_gpu_stream_create(stream, picture.sps, decode.surfaces_needed);
            h264_stream_set_surfaces(state, stream->surfaces.count);
            if (!created) {
                if (state->unsupported)
                    h264_stream_lost(state);
                else
                    h264_stream_reset(state);
                continue;
            }
            saturating_increment(stream->stats.reinitializations);
        }
        const H264Sps& sps = picture.sps;
        if (picture.first.idr)
            stream_set_format(stream, sps.crop_x, sps.crop_y, sps.width, sps.height, sps.matrix, sps.full_range,
                              sps.sar_width, sps.sar_height);
        if (!h264_gpu_stream_submit(stream, picture, decode, bytes)) {
            h264_stream_reset(state);
            stream_request_keyframe(stream, state->unsupported);
            continue;
        }
        h264_stream_commit(state, picture, decode, rtp_timestamp);
        stream->last_request_ms = 0; // A keyframe arrived or the stream is healthy.
        present_ready(stream);
    }
}

void h264_gpu_stream_stats(const GpuStream& stream, GpuStreamDecoderStats* stats) noexcept {
    const H264StreamState& state = stream.h264.state;
    stats->decoded = state.decoded;
    stats->dropped = state.dropped;
    stats->damaged = state.damaged;
    stats->gaps = state.gaps;
    stats->unsupported = state.unsupported;
}

} // namespace maicord
#pragma clang attribute pop
