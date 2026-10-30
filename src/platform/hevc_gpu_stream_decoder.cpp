#include "gpu_stream_decoder_backend.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/dxva.hpp"
#include "hevc_dxva_parameters.hpp"
// DXVA submission, once per frame and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {

void present_ready(GpuStream* stream) noexcept {
    H264StreamOutput output{};
    while (hevc_stream_pop(&stream->hevc.state, &output))
        gpu_stream_present(stream, output.surface, output.rtp_timestamp, output.keyframe);
}

void request_keyframe(GpuStream* stream) noexcept {
    gpu_stream_request_keyframe(stream, stream->hevc.state.unsupported);
}

// DXVA HEVC surfaces are allocated in 64-pixel units; the decoder and the
// conformance crop keep the coded size.
u32 surface_extent(u32 coded) noexcept {
    return (coded + 63u) & ~63u;
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
    if (stream->stats.device_lost) {
        saturating_increment(stream->stats.dropped);
        return;
    }
    auto scanner = hevc_stream_scanner(bytes);
    for (;;) {
        const HevcScanStatus scanned = hevc_stream_scan(&state->sets, &scanner, &picture);
        if (scanned == HevcScanStatus::end)
            break;
        if (scanned == HevcScanStatus::damaged || !intact) {
            ++state->damaged;
            // An SPS this decoder refuses (Main10, 4:4:4): no keyframe can help.
            if (picture.error == HevcError::unsupported &&
                state->sets.last_sps_error == HevcError::unsupported)
                state->unsupported = true;
            if (state->active && !state->waiting)
                hevc_stream_lost(state);
            request_keyframe(stream);
            continue;
        }
        state->held = gpu_stream_held(*stream);
        HevcStreamDecode decode{};
        const HevcStreamAction action = hevc_stream_prepare(state, picture, &decode);
        if (action == HevcStreamAction::unsupported || action == HevcStreamAction::skip)
            continue;
        if (action != HevcStreamAction::decode) {
            request_keyframe(stream);
            continue;
        }
        present_ready(stream); // Drain before an IRAP or a surface replacement.
        const HevcConfiguration& config = state->sets.active;
        const HevcSps& sps = config.sps;
        if (decode.reinitialize) {
            const HRESULT status = gpu_stream_create_decoder(stream, {&D3D11_DECODER_PROFILE_HEVC_VLD_MAIN,
                                                                      sps.coded_width,
                                                                      sps.coded_height,
                                                                      surface_extent(sps.coded_width),
                                                                      surface_extent(sps.coded_height),
                                                                      {60, 1},
                                                                      1,
                                                                      decode.surfaces_needed});
            hevc_stream_set_surfaces(state, stream->surfaces.count);
            if (FAILED(status)) {
                stream->stats.last_status = status;
                if (status == DXGI_ERROR_UNSUPPORTED || status == E_INVALIDARG)
                    state->unsupported = true;
                else
                    gpu_stream_failure(stream, status);
                hevc_stream_lost(state);
                continue;
            }
            saturating_increment(stream->stats.reinitializations);
        }
        if (picture.header.irap)
            gpu_stream_set_format(stream, sps.crop_x, sps.crop_y, sps.width, sps.height, sps.matrix,
                                  sps.full_range, sps.sar_width, sps.sar_height);
        DXVA_PicParams_HEVC parameters;
        DXVA_Qmatrix_HEVC matrix{};
        const HevcReferenceState none{};
        if (!hevc_dxva_picture_parameters(sps, config.pps, picture.header, picture.intra,
                                          decode.restart ? none : state->references, decode.plan,
                                          decode.surface, stream->feedback, &parameters, &matrix)) {
            ++state->damaged;
            hevc_stream_lost(state);
            request_keyframe(stream);
            continue;
        }
        const HRESULT status = gpu_stream_submit_slices(
            stream, decode.surface, {reinterpret_cast<const u8*>(&parameters), sizeof(parameters)},
            {reinterpret_cast<const u8*>(&matrix), sps.scaling_enabled ? u32{sizeof(matrix)} : 0u}, bytes,
            picture.slices, picture.slice_count);
        if (FAILED(status)) {
            gpu_stream_failure(stream, status);
            hevc_stream_lost(state);
            request_keyframe(stream);
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
