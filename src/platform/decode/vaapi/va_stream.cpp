#include "../linux/stream.hpp"
#include "va.hpp"
#include "va_parameters.hpp"

// Per-picture control flow, no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

// The VA-API half of the hardware tier of the Linux live stream decoder: the codec backends of both systems
// (*_gpu_stream_decoder.cpp, av1_gpu_stream.cpp) submit through it. A picture goes into a VASurface and is not waited
// for until it is shown: then it is synced, exported again (drivers differ in when a surface's dma-buf is complete,
// and a fresh export is always right) and its two dma-buf planes imported by the renderer's EGL, or, on drivers with
// stable exports, the images of the surface's previous export are reused.

namespace maicord {
namespace {

using namespace vaapi;

// Pictures the GPU decoder may fail in a row before the stream is handed to the software tier.
constexpr u8 kFailuresBeforeFallback = 3;

Session* session_of(GpuStream* stream) noexcept {
    return static_cast<Session*>(stream->session);
}

void release_hardware(GpuStream* stream) noexcept {
    session_destroy(session_of(stream));
    stream->session = nullptr;
    stream_surfaces_release(stream);
}

// Allocates `count` surfaces of width x height (P010 for ten_bit); the stream's slots stay empty until a picture is
// shown in them.
bool create_hardware(GpuStream* stream, i32 profile, u32 width, u32 height, u32 count, u32 coded_width,
                     u32 coded_height, bool ten_bit = false) noexcept {
    release_hardware(stream);
    const Driver* driver_ = driver(stream->device);
    if (!driver_ || !profile)
        return false;
    Session* session = session_create(driver_, profile, width, height, count, ten_bit);
    if (!session)
        return false;
    stream->session = session;
    stream->surfaces.count = count;
    stream->surfaces.width = width;
    stream->surfaces.height = height;
    stream->coded_width = coded_width;
    stream->coded_height = coded_height;
    return true;
}

// A picture the GPU decoder failed: a few in a row give the stream to the software tier (*unsupported).
void hardware_failure(GpuStream* stream, bool* unsupported) noexcept {
    saturating_increment(stream->stats.gpu_failures);
    if (++stream->hardware_failures >= kFailuresBeforeFallback)
        *unsupported = true;
}

// Surfaces are allocated in 64-pixel units (the coding tree block); the picture keeps its coded size.
u32 hevc_surface_extent(u32 coded) noexcept {
    return (coded + 63u) & ~63u;
}

// Hardware decode here is 4:2:0: 8-bit into NV12, and 10-bit into P010 on a driver whose probe decoded and imported
// one. The conversion shader reads a 10-bit picture's top 8 bits (to the nearest level) and applies the stream's matrix
// without tone mapping, so a BT.2020 stream (HDR) is refused, as every 10-bit stream was before P010.
bool av1_ten_bit_ok(const Av1SequenceHeader& sequence, const Driver* d) noexcept {
    constexpr u8 kBt2020NonConstant = 9, kBt2020Constant = 10; // matrix_coefficients
    return sequence.bit_depth == 10 && d && (d->ten_bit & gpu_stream_codec_bit(GpuStreamCodec::av1)) &&
           sequence.matrix_coefficients != kBt2020NonConstant && sequence.matrix_coefficients != kBt2020Constant;
}

// Indexed by GpuStreamCodec.
const StreamBackend kBackends[]{
    {true, h264_gpu_stream_reset, h264_gpu_stream_decode, h264_gpu_stream_drain, release_hardware,
     h264_gpu_stream_stats},
    {true, hevc_gpu_stream_reset, hevc_gpu_stream_decode, hevc_gpu_stream_drain, release_hardware,
     hevc_gpu_stream_stats},
    {true, av1_gpu_stream_reset, av1_gpu_stream_decode, av1_gpu_stream_drain, release_hardware, av1_gpu_stream_stats}};

} // namespace

// Replaces the planes of slot with the export of its surface (the same images again while the driver's exports are
// stable) and publishes it. A picture the GPU decoded, exported and imported ends the run of failures that gives the
// tier up.
bool stream_show(GpuStream* stream, u8 slot, u32 rtp_timestamp, bool keyframe, bool* unsupported) noexcept {
    if (slot >= stream->surfaces.count || !session_of(stream) ||
        !stream_slot_export(stream->device, session_of(stream), slot, &stream->surfaces.slot[slot])) {
        hardware_failure(stream, unsupported);
        return false;
    }
    stream->hardware_failures = 0;
    stream_present(stream, slot, rtp_timestamp, keyframe);
    return true;
}

// ---- H.264 ------------------------------------------------------------------------------------------------------

bool h264_gpu_stream_create(GpuStream* stream, const H264Sps& sps, u8 surfaces) noexcept {
    const Driver* d = driver(stream->device);
    if (d && sps.coded_width <= d->max_width[0] && sps.coded_height <= d->max_height[0] &&
        create_hardware(stream, h264_profile_for(*d, sps.profile), sps.coded_width, sps.coded_height, surfaces,
                        sps.coded_width, sps.coded_height))
        return true;
    stream->h264.state.unsupported = true; // a profile, size or memory the GPU decoder cannot give
    return false;
}

// All slices of the picture in one VA picture; false when a header does not parse or a buffer is refused.
bool h264_gpu_stream_submit(GpuStream* stream, const H264StreamPicture& picture, const H264StreamDecode& decode,
                            ConstSlice<u8> bytes) noexcept {
    Session* session = session_of(stream);
    H264StreamState& state = stream->h264.state;
    bool ok = picture_begin(session, decode.surface);
    if (ok) {
        PictureParameterH264 parameters;
        h264_picture_parameters(picture.sps, picture.pps, picture.first, state.references, decode.plan.decode_order,
                                decode.surface, session->surface, &parameters);
        IqMatrixH264 matrix;
        h264_iq_matrix(picture.pps, &matrix);
        ok = picture_buffer(session, kPictureParameterBuffer, &parameters, sizeof(parameters)) &&
             picture_buffer(session, kIqMatrixBuffer, &matrix, sizeof(matrix));
        for (u32 i = 0; ok && i < picture.slice_count; ++i) {
            const ConstSlice<u8> nal{bytes.data + picture.slices[i].offset, picture.slices[i].size};
            H264SliceDetail detail;
            SliceParameterH264 slice;
            ok = h264_parse_slice_detail(nal, picture.sps, picture.pps, &detail) == H264Error::none &&
                 h264_slice_parameters(picture.sps, picture.pps, picture.first, detail, state.references,
                                       decode.plan.decode_order, session->surface, nal.count, &slice) &&
                 picture_slice(session, &slice, sizeof(slice), nal.data, nal.count);
        }
        ok = picture_end(session) && ok;
    }
    if (!ok)
        hardware_failure(stream, &state.unsupported);
    return ok;
}

// ---- HEVC -------------------------------------------------------------------------------------------------------

bool hevc_gpu_stream_create(GpuStream* stream, const HevcSps& sps, u8 surfaces) noexcept {
    const Driver* d = driver(stream->device);
    if (d && sps.coded_width <= d->max_width[1] && sps.coded_height <= d->max_height[1] &&
        create_hardware(stream, kProfileHevcMain, hevc_surface_extent(sps.coded_width),
                        hevc_surface_extent(sps.coded_height), surfaces, sps.coded_width, sps.coded_height))
        return true;
    stream->hevc.state.unsupported = true;
    return false;
}

bool hevc_gpu_stream_submit(GpuStream* stream, const HevcStreamPicture& picture, const HevcStreamDecode& decode,
                            ConstSlice<u8> bytes) noexcept {
    Session* session = session_of(stream);
    HevcStreamState& state = stream->hevc.state;
    const HevcConfiguration& config = state.sets.active;
    const HevcReferenceState none{};
    HevcGpuReferences references{};
    bool ok = hevc_gpu_reference_map(decode.restart ? none : state.references, decode.plan, decode.surface,
                                     &references) &&
              picture_begin(session, decode.surface);
    if (ok) {
        PictureParameterHevc parameters;
        hevc_picture_parameters(config.sps, config.pps, picture.header, picture.intra, decode.plan, references,
                                decode.surface, session->surface, &parameters);
        ok = picture_buffer(session, kPictureParameterBuffer, &parameters, sizeof(parameters));
        if (ok && config.sps.scaling_enabled) {
            IqMatrixHevc matrix;
            hevc_iq_matrix(config.pps, &matrix);
            ok = picture_buffer(session, kIqMatrixBuffer, &matrix, sizeof(matrix));
        }
        HevcSliceDetail previous{}, detail{};
        for (u32 i = 0; ok && i < picture.slice_count; ++i) {
            const ConstSlice<u8> nal{bytes.data + picture.slices[i].offset, picture.slices[i].size};
            HevcNal unit{nal, HevcError::none, static_cast<u8>((nal[0] >> 1) & 63u),
                         static_cast<u8>((nal[1] & 7u) - 1u)};
            SliceParameterHevc slice;
            ok = hevc_parse_slice_detail(unit, config, i ? &previous : nullptr, &detail) == HevcError::none;
            if (!ok)
                break;
            hevc_slice_parameters(detail, references, i + 1 == picture.slice_count, nal.count, &slice);
            ok = picture_slice(session, &slice, sizeof(slice), nal.data, nal.count);
            previous = detail;
        }
        ok = picture_end(session) && ok;
    }
    if (!ok) {
        ++state.damaged;
        hardware_failure(stream, &state.unsupported);
    }
    return ok;
}

// ---- AV1 --------------------------------------------------------------------------------------------------------

bool av1_gpu_stream_decodable(const GpuStream& stream, const Av1SequenceHeader& sequence) noexcept {
    const Driver* d = driver(stream.device);
    return sequence.profile == 0 && (sequence.bit_depth == 8 || av1_ten_bit_ok(sequence, d)) && !sequence.mono_chrome &&
           sequence.subsampling_x && sequence.subsampling_y && sequence.max_width <= 4096 && sequence.max_height <= 4096;
}

bool av1_gpu_stream_create(GpuStream* stream) noexcept {
    Av1GpuStreamState* state = &stream->av1;
    const Av1SequenceHeader& sequence = state->sequence;
    const Driver* d = driver(stream->device);
    if (d && sequence.max_width <= d->max_width[2] && sequence.max_height <= d->max_height[2]) {
        // Surfaces cover whole superblocks of the largest frame.
        const u32 align = sequence.use_128x128_superblock ? 127u : 63u;
        if (create_hardware(stream, kProfileAv1Profile0, (sequence.max_width + align) & ~align,
                            (sequence.max_height + align) & ~align, state->surfaces_wanted, sequence.max_width,
                            sequence.max_height, sequence.bit_depth == 10))
            return true;
    }
    state->unsupported = true;
    return false;
}

// One frame into surface: the picture parameters, then every tile of the temporal unit in one data buffer.
bool av1_gpu_stream_submit(GpuStream* stream, u8 surface, ConstSlice<u8> unit) noexcept {
    Session* session = session_of(stream);
    Av1GpuStreamState& state = stream->av1;
    bool ok = state.tile_count && picture_begin(session, surface);
    if (ok) {
        PictureParameterAv1 parameters;
        av1_picture_parameters(state.sequence, state.header, state.slot_surface, session->surface, surface,
                               &parameters);
        ok = picture_buffer(session, kPictureParameterBuffer, &parameters, sizeof(parameters));
        u32 start = state.tiles[0].offset, end = start;
        for (u32 i = 0; i < state.tile_count; ++i) {
            const Av1Tile& tile = state.tiles[i];
            if (tile.offset < start)
                start = tile.offset;
            if (tile.offset + tile.size > end)
                end = tile.offset + tile.size;
        }
        ok = ok && end <= unit.count;
        // One slice data buffer holds every tile; each parameter buffer names its tile's extent within it. The data
        // follows the first parameter buffer, as libva's own users submit it.
        for (u32 i = 0; ok && i < state.tile_count; ++i) {
            SliceParameterAv1 slice;
            av1_tile_parameters(state.tiles[i], state.tiles[i].offset - start, 0, state.tile_count - 1, &slice);
            ok = picture_slice_parameters(session, &slice, sizeof(slice));
            if (ok && i == 0)
                ok = picture_slice_data(session, unit.data + start, end - start);
        }
        ok = picture_end(session) && ok;
    }
    if (!ok)
        hardware_failure(stream, &state.unsupported);
    return ok;
}

// ---- The tier ---------------------------------------------------------------------------------------------------

u32 hardware_probe_codecs(GpuDevice* device) noexcept {
    const Driver* d = driver(device);
    return d ? d->codecs : 0;
}

const StreamBackend* hardware_stream_backend(GpuStreamCodec codec) noexcept {
    return static_cast<u8>(codec) < sizeof(kBackends) / sizeof(kBackends[0]) ? &kBackends[static_cast<u8>(codec)]
                                                                            : nullptr;
}

} // namespace maicord
#pragma clang attribute pop
