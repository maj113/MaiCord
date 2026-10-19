#include "gpu_stream_decoder_backend.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/dxva.hpp"
#include "h264_dxva_parameters.hpp"

namespace maicord {
namespace {

void present_ready(GpuStream* stream) noexcept {
    H264StreamOutput output{};
    while (h264_stream_pop(&stream->h264.state, &output))
        gpu_stream_present(stream, output.surface, output.rtp_timestamp, output.keyframe);
}

void failure(GpuStream* stream, HRESULT status) noexcept {
    gpu_stream_failure(stream, status);
    h264_stream_reset(&stream->h264.state);
}

void request_keyframe(GpuStream* stream) noexcept {
    gpu_stream_request_keyframe(stream, stream->h264.state.unsupported);
}

HRESULT create_decoder(GpuStream* stream, const H264Sps& sps, u8 surfaces) noexcept {
    DXGI_RATIONAL rate{60, 1};
    if (sps.time_scale && sps.units_in_tick && sps.units_in_tick < 0x80000000u)
        rate = {sps.time_scale, sps.units_in_tick * 2u};
    return gpu_stream_create_decoder(stream, {&D3D11_DECODER_PROFILE_H264_VLD_NOFGT, sps.coded_width,
                                              sps.coded_height, sps.coded_width, sps.coded_height, rate, 2,
                                              surfaces});
}

} // namespace

void h264_gpu_stream_reset(H264GpuStreamState* backend) noexcept {
    h264_stream_reset(&backend->state);
    h264_stream_set_surfaces(&backend->state, 0);
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
    if (stream->stats.device_lost) {
        saturating_increment(stream->stats.dropped);
        return;
    }
    auto scanner = h264_stream_scanner(bytes);
    for (;;) {
        const H264ScanStatus scanned = h264_stream_scan(&state->sets, &scanner, &picture);
        if (scanned == H264ScanStatus::end)
            break;
        if (scanned == H264ScanStatus::damaged || !intact) {
            ++state->damaged;
            if (state->active && !state->waiting)
                h264_stream_lost(state);
            request_keyframe(stream);
            continue;
        }
        state->held = gpu_stream_held(*stream);
        H264StreamDecode decode{};
        const H264StreamAction action = h264_stream_prepare(state, picture, &decode);
        if (action == H264StreamAction::unsupported)
            continue;
        if (action != H264StreamAction::decode) {
            request_keyframe(stream);
            continue;
        }
        present_ready(stream); // Drain before an IDR or a surface replacement.
        if (decode.reinitialize) {
            const HRESULT status = create_decoder(stream, picture.sps, decode.surfaces_needed);
            h264_stream_set_surfaces(state, stream->surfaces.count);
            if (FAILED(status)) {
                stream->stats.last_status = status;
                if (status == DXGI_ERROR_UNSUPPORTED || status == E_INVALIDARG) {
                    state->unsupported = true;
                    h264_stream_lost(state);
                } else
                    failure(stream, status);
                continue;
            }
            saturating_increment(stream->stats.reinitializations);
        }
        const H264Sps& sps = picture.sps;
        if (picture.first.idr)
            gpu_stream_set_format(stream, sps.crop_x, sps.crop_y, sps.width, sps.height, sps.matrix,
                                  sps.full_range, sps.sar_width, sps.sar_height);
        DXVA_PicParams_H264 parameters{};
        DXVA_Qmatrix_H264 matrix{};
        h264_dxva_picture_parameters(sps, picture.pps, picture.first, picture.all_intra, state->references,
                                     decode.plan.decode_order, decode.surface, stream->feedback,
                                     &parameters, &matrix);
        const HRESULT status = gpu_stream_submit(
            stream, decode.surface, {reinterpret_cast<const u8*>(&parameters), sizeof(parameters)},
            {reinterpret_cast<const u8*>(&matrix), sizeof(matrix)}, bytes, picture.slices,
            picture.slice_count);
        if (FAILED(status)) {
            failure(stream, status);
            request_keyframe(stream);
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
    stats->late = state.late;
    stats->unsupported = state.unsupported;
}

} // namespace maicord
