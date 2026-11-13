#include "gpu_stream_decoder_backend.hpp"
// The HEVC backend of both systems over their submission (hevc_gpu_stream_create, hevc_gpu_stream_submit): once
// per frame and no byte loop, compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {

void present_ready(GpuStream* stream) noexcept {
    HevcStreamState* state = &stream->hevc.state;
    H264StreamOutput output{};
    while (hevc_stream_pop(state, &output))
        if (!stream_show(stream, output.surface, output.rtp_timestamp, output.keyframe, &state->unsupported))
            hevc_stream_lost(state);
}

} // namespace

void hevc_gpu_stream_reset(GpuStream* stream) noexcept {
    hevc_stream_reset(&stream->hevc.state);
    hevc_stream_set_surfaces(&stream->hevc.state, 0);
}

void hevc_gpu_stream_drain(GpuStream* stream) noexcept {
    stream->hevc.state.drain = true;
    present_ready(stream);
}

void hevc_gpu_stream_decode(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact,
                            bool resynchronize) noexcept {
    HevcStreamState* state = &stream->hevc.state;
    HevcStreamPicture& picture = stream->hevc.picture;
    if (resynchronize)
        hevc_stream_lost(state);
#if defined(_WIN32)
    if (stream->stats.device_lost) // Only DXVA reports a lost device.
        return;
#endif
    auto scanner = hevc_stream_scanner(bytes);
    for (;;) {
        const HevcScanStatus scanned = hevc_stream_scan(&state->sets, &scanner, &picture);
        if (scanned == HevcScanStatus::end)
            break;
        if (scanned == HevcScanStatus::damaged || !intact) {
            ++state->damaged;
            // An SPS this decoder refuses (Main10, 4:4:4): no keyframe can help.
            if (picture.error == HevcError::unsupported && state->sets.last_sps_error == HevcError::unsupported)
                state->unsupported = true;
            if (state->active && !state->waiting)
                hevc_stream_lost(state);
            stream_request_keyframe(stream, state->unsupported);
            continue;
        }
        state->held = stream_held(*stream);
        HevcStreamDecode decode{};
        const HevcStreamAction action = hevc_stream_prepare(state, picture, &decode);
        if (action == HevcStreamAction::unsupported || action == HevcStreamAction::skip)
            continue;
        if (action != HevcStreamAction::decode) {
            stream_request_keyframe(stream, state->unsupported);
            continue;
        }
        present_ready(stream); // Drain before an IRAP or a surface replacement.
        const HevcSps& sps = state->sets.active.sps;
        if (decode.reinitialize) {
            const bool created = hevc_gpu_stream_create(stream, sps, decode.surfaces_needed);
            hevc_stream_set_surfaces(state, stream->surfaces.count);
            if (!created) {
                hevc_stream_lost(state);
                continue;
            }
            saturating_increment(stream->stats.reinitializations);
        }
        if (picture.header.irap)
            stream_set_format(stream, sps.crop_x, sps.crop_y, sps.width, sps.height, sps.matrix, sps.full_range,
                              sps.sar_width, sps.sar_height);
        if (!hevc_gpu_stream_submit(stream, picture, decode, bytes)) {
            hevc_stream_lost(state);
            stream_request_keyframe(stream, state->unsupported);
            continue;
        }
        hevc_stream_commit(state, picture, decode, rtp_timestamp);
        stream->last_request_ms = 0; // A keyframe arrived or the stream is healthy.
        present_ready(stream);
    }
}

void hevc_gpu_stream_stats(const GpuStream& stream, GpuStreamDecoderStats* stats) noexcept {
    const HevcStreamState& state = stream.hevc.state;
    stats->decoded = state.decoded;
    stats->dropped = state.dropped;
    stats->damaged = state.damaged;
    stats->gaps = state.gaps;
    stats->unsupported = state.unsupported;
}

} // namespace maicord
#pragma clang attribute pop
