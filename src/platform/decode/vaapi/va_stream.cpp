#include "../linux/stream.hpp"
#include "va.hpp"
#include "va_parameters.hpp"

// Per-picture control flow, no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

// The hardware tier of the Linux live stream decoder: the codec state machines of src/media/video (what
// src/platform/decode/*_gpu_stream_decoder.cpp drives on Windows) with VA-API as the submission. A picture goes into
// a VASurface and is not waited for until it is shown: then it is synced, exported again (drivers differ in when a
// surface's dma-buf is complete, and a fresh export is always right) and its two dma-buf planes imported by the
// renderer's EGL, or, on drivers with stable exports, the images of the surface's previous export are reused.

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

// Replaces the planes of slot with the export of its surface (the same images again while the driver's exports are
// stable) and publishes it. A picture the GPU decoded, exported and imported ends the run of failures that gives the
// tier up.
bool hardware_present(GpuStream* stream, u8 slot, u32 rtp_timestamp, bool keyframe) noexcept {
    if (slot >= stream->surfaces.count || !session_of(stream) ||
        !stream_slot_export(stream->device, session_of(stream), slot, &stream->surfaces.slot[slot]))
        return false;
    stream->hardware_failures = 0;
    stream_present(stream, slot, rtp_timestamp, keyframe);
    return true;
}

// ---- H.264 ------------------------------------------------------------------------------------------------------

namespace h264 {

void failure(GpuStream* stream) noexcept {
    saturating_increment(stream->stats.gpu_failures);
    if (++stream->hardware_failures >= kFailuresBeforeFallback)
        stream->h264.state.unsupported = true;
    h264_stream_reset(&stream->h264.state);
}

void present_ready(GpuStream* stream) noexcept {
    H264StreamOutput output{};
    while (h264_stream_pop(&stream->h264.state, &output))
        if (!hardware_present(stream, output.surface, output.rtp_timestamp, output.keyframe))
            failure(stream);
}

void reset(GpuStream* stream) noexcept {
    h264_stream_reset(&stream->h264.state);
    h264_stream_set_surfaces(&stream->h264.state, 0);
}

void drain(GpuStream* stream) noexcept {
    stream->h264.state.drain = true;
    present_ready(stream);
}

bool create(GpuStream* stream, const H264Sps& sps, u8 surfaces) noexcept {
    const Driver* d = driver(stream->device);
    if (!d || sps.coded_width > d->max_width[0] || sps.coded_height > d->max_height[0])
        return false;
    return create_hardware(stream, h264_profile_for(*d, sps.profile), sps.coded_width, sps.coded_height, surfaces,
                           sps.coded_width, sps.coded_height);
}

// All slices of the picture in one VA picture; false when a header does not parse or a buffer is refused.
bool submit(GpuStream* stream, const H264StreamPicture& picture, const H264StreamDecode& decode,
            ConstSlice<u8> bytes) noexcept {
    Session* session = session_of(stream);
    const H264StreamState& state = stream->h264.state;
    if (!picture_begin(session, decode.surface))
        return false;
    PictureParameterH264 parameters;
    h264_picture_parameters(picture.sps, picture.pps, picture.first, state.references, decode.plan.decode_order,
                            decode.surface, session->surface, &parameters);
    IqMatrixH264 matrix;
    h264_iq_matrix(picture.pps, &matrix);
    bool ok = picture_buffer(session, kPictureParameterBuffer, &parameters, sizeof(parameters)) &&
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
    return picture_end(session) && ok;
}

void decode(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact, bool resynchronize) noexcept {
    H264StreamState* state = &stream->h264.state;
    H264StreamPicture& picture = stream->h264.picture;
    if (resynchronize)
        h264_stream_lost(state);
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
            const bool created = create(stream, picture.sps, decode.surfaces_needed);
            h264_stream_set_surfaces(state, stream->surfaces.count);
            if (!created) {
                state->unsupported = true; // a profile, size or memory the GPU decoder cannot give
                h264_stream_lost(state);
                continue;
            }
            saturating_increment(stream->stats.reinitializations);
        }
        const H264Sps& sps = picture.sps;
        if (picture.first.idr)
            stream_set_format(stream, sps.crop_x, sps.crop_y, sps.width, sps.height, sps.matrix, sps.full_range,
                              sps.sar_width, sps.sar_height);
        if (!submit(stream, picture, decode, bytes)) {
            failure(stream);
            stream_request_keyframe(stream, state->unsupported);
            continue;
        }
        h264_stream_commit(state, picture, decode, rtp_timestamp);
        stream->last_request_ms = 0; // A keyframe arrived or the stream is healthy.
        present_ready(stream);
    }
}

void stats(const GpuStream& stream, GpuStreamDecoderStats* out) noexcept {
    const H264StreamState& state = stream.h264.state;
    out->decoded = state.decoded;
    out->dropped = state.dropped;
    out->damaged = state.damaged;
    out->gaps = state.gaps;
    out->unsupported = state.unsupported;
}

} // namespace h264

// ---- HEVC -------------------------------------------------------------------------------------------------------

namespace hevc {

// Surfaces are allocated in 64-pixel units (the coding tree block); the picture keeps its coded size.
u32 surface_extent(u32 coded) noexcept {
    return (coded + 63u) & ~63u;
}

void failure(GpuStream* stream) noexcept {
    saturating_increment(stream->stats.gpu_failures);
    if (++stream->hardware_failures >= kFailuresBeforeFallback)
        stream->hevc.state.unsupported = true;
    hevc_stream_lost(&stream->hevc.state);
}

void present_ready(GpuStream* stream) noexcept {
    H264StreamOutput output{};
    while (hevc_stream_pop(&stream->hevc.state, &output))
        if (!hardware_present(stream, output.surface, output.rtp_timestamp, output.keyframe))
            failure(stream);
}

void reset(GpuStream* stream) noexcept {
    hevc_stream_reset(&stream->hevc.state);
    hevc_stream_set_surfaces(&stream->hevc.state, 0);
}

void drain(GpuStream* stream) noexcept {
    stream->hevc.state.drain = true;
    present_ready(stream);
}

bool create(GpuStream* stream, const HevcSps& sps, u8 surfaces) noexcept {
    const Driver* d = driver(stream->device);
    if (!d || sps.coded_width > d->max_width[1] || sps.coded_height > d->max_height[1])
        return false;
    return create_hardware(stream, kProfileHevcMain, surface_extent(sps.coded_width), surface_extent(sps.coded_height),
                           surfaces, sps.coded_width, sps.coded_height);
}

bool submit(GpuStream* stream, const HevcStreamPicture& picture, const HevcStreamDecode& decode, bool restart,
            ConstSlice<u8> bytes) noexcept {
    Session* session = session_of(stream);
    const HevcStreamState& state = stream->hevc.state;
    const HevcConfiguration& config = state.sets.active;
    const HevcReferenceState none{};
    HevcGpuReferences references{};
    if (!hevc_gpu_reference_map(restart ? none : state.references, decode.plan, decode.surface, &references))
        return false;
    if (!picture_begin(session, decode.surface))
        return false;
    PictureParameterHevc parameters;
    hevc_picture_parameters(config.sps, config.pps, picture.header, picture.intra, decode.plan, references,
                            decode.surface, session->surface, &parameters);
    bool ok = picture_buffer(session, kPictureParameterBuffer, &parameters, sizeof(parameters));
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
    return picture_end(session) && ok;
}

void decode(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact, bool resynchronize) noexcept {
    HevcStreamState* state = &stream->hevc.state;
    HevcStreamPicture& picture = stream->hevc.picture;
    if (resynchronize)
        hevc_stream_lost(state);
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
        const HevcConfiguration& config = state->sets.active;
        const HevcSps& sps = config.sps;
        if (decode.reinitialize) {
            const bool created = create(stream, sps, decode.surfaces_needed);
            hevc_stream_set_surfaces(state, stream->surfaces.count);
            if (!created) {
                state->unsupported = true;
                hevc_stream_lost(state);
                continue;
            }
            saturating_increment(stream->stats.reinitializations);
        }
        if (picture.header.irap)
            stream_set_format(stream, sps.crop_x, sps.crop_y, sps.width, sps.height, sps.matrix, sps.full_range,
                              sps.sar_width, sps.sar_height);
        if (!submit(stream, picture, decode, decode.restart, bytes)) {
            ++state->damaged;
            failure(stream);
            stream_request_keyframe(stream, state->unsupported);
            continue;
        }
        hevc_stream_commit(state, picture, decode, rtp_timestamp);
        stream->last_request_ms = 0; // A keyframe arrived or the stream is healthy.
        present_ready(stream);
    }
}

void stats(const GpuStream& stream, GpuStreamDecoderStats* out) noexcept {
    const HevcStreamState& state = stream.hevc.state;
    out->decoded = state.decoded;
    out->dropped = state.dropped;
    out->damaged = state.damaged;
    out->gaps = state.gaps;
    out->unsupported = state.unsupported;
}

} // namespace hevc

// ---- AV1 --------------------------------------------------------------------------------------------------------

namespace av1 {

void damaged(GpuStream* stream) noexcept {
    Av1GpuStreamState* state = &stream->av1;
    saturating_increment(state->damaged);
    av1_stream_lose(state);
    stream_request_keyframe(stream, state->unsupported);
}

void failure(GpuStream* stream) noexcept {
    saturating_increment(stream->stats.gpu_failures);
    if (++stream->hardware_failures >= kFailuresBeforeFallback)
        stream->av1.unsupported = true;
}

// Hardware decode here is 4:2:0: 8-bit into NV12, and 10-bit into P010 on a driver whose probe decoded and imported
// one. The conversion shader reads a 10-bit picture's top 8 bits (to the nearest level) and applies the stream's matrix
// without tone mapping, so a BT.2020 stream (HDR) is refused, as every 10-bit stream was before P010.
bool ten_bit_ok(const Av1SequenceHeader& sequence, const Driver* d) noexcept {
    constexpr u8 kBt2020NonConstant = 9, kBt2020Constant = 10; // matrix_coefficients
    return sequence.bit_depth == 10 && d && (d->ten_bit & gpu_stream_codec_bit(GpuStreamCodec::av1)) &&
           sequence.matrix_coefficients != kBt2020NonConstant && sequence.matrix_coefficients != kBt2020Constant;
}

bool decodable(const Av1SequenceHeader& sequence, const Driver* d) noexcept {
    return sequence.profile == 0 && (sequence.bit_depth == 8 || ten_bit_ok(sequence, d)) && !sequence.mono_chrome &&
           sequence.subsampling_x && sequence.subsampling_y && sequence.max_width <= 4096 && sequence.max_height <= 4096;
}

bool create(GpuStream* stream) noexcept {
    Av1GpuStreamState* state = &stream->av1;
    const Av1SequenceHeader& sequence = state->sequence;
    const Driver* d = driver(stream->device);
    bool created = false;
    if (d && sequence.max_width <= d->max_width[2] && sequence.max_height <= d->max_height[2]) {
        // Surfaces cover whole superblocks of the largest frame.
        const u32 align = sequence.use_128x128_superblock ? 127u : 63u;
        created = create_hardware(stream, kProfileAv1Profile0, (sequence.max_width + align) & ~align,
                                  (sequence.max_height + align) & ~align, state->surfaces_wanted, sequence.max_width,
                                  sequence.max_height, sequence.bit_depth == 10);
    }
    av1_stream_clear_references(state);
    state->sequence_changed = false;
    return created;
}

// One frame into surface: the picture parameters, then every tile of the temporal unit in one data buffer.
bool submit(GpuStream* stream, u8 surface, ConstSlice<u8> unit) noexcept {
    Session* session = session_of(stream);
    const Av1GpuStreamState& state = stream->av1;
    if (!state.tile_count || !picture_begin(session, surface))
        return false;
    PictureParameterAv1 parameters;
    av1_picture_parameters(state.sequence, state.header, state.slot_surface, session->surface, surface, &parameters);
    bool ok = picture_buffer(session, kPictureParameterBuffer, &parameters, sizeof(parameters));
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
    return picture_end(session) && ok;
}

void present(GpuStream* stream, u8 surface, const Av1FrameHeader& header, u32 rtp_timestamp) noexcept {
    const Av1SequenceHeader& sequence = stream->av1.sequence;
    // render_size crops a coded frame that is larger than the picture.
    const u32 width = minimum(header.render_width, header.upscaled_width);
    const u32 height = minimum(header.render_height, header.frame_height);
    if (header.frame_type == av1_key_frame || stream->frame.width != width || stream->frame.height != height ||
        !stream->frame.texture)
        stream_set_format(stream, 0, 0, width, height, sequence.matrix_coefficients, sequence.color_range, 1, 1);
    if (!hardware_present(stream, surface, rtp_timestamp, header.frame_type == av1_key_frame)) {
        failure(stream);
        damaged(stream);
    }
}

void decode_frame(GpuStream* stream, ConstSlice<u8> unit, u32 rtp_timestamp) noexcept {
    Av1GpuStreamState* state = &stream->av1;
    const Av1FrameHeader& header = state->header;
    state->frame_open = false;
    if (state->waiting && header.frame_type != av1_key_frame) {
        saturating_increment(state->dropped);
        stream_request_keyframe(stream, state->unsupported);
        return;
    }
    // The VA decoder shows reference pictures without film grain: such a stream is the software tier's.
    if (header.film_grain.apply_grain && state->sequence.film_grain_params_present) {
        state->unsupported = true;
        av1_stream_lose(state);
        return;
    }
    if (header.frame_type == av1_key_frame &&
        (!stream->session || state->sequence_changed || stream->surfaces.count < state->surfaces_wanted)) {
        if (!create(stream)) {
            state->unsupported = true;
            av1_stream_lose(state);
            return;
        }
        saturating_increment(stream->stats.reinitializations);
    } else if (state->waiting) {
        // Decoding restarts at this keyframe: earlier slots are not trusted.
        av1_stream_clear_references(state);
    }
    const u8 surface = av1_stream_free_surface(*state, stream_held(*stream), stream->surfaces.count);
    if (surface == kAv1NoSurface) {
        // More distinct references than the pool holds: grow at the next keyframe.
        state->surfaces_wanted = kAv1RefFrames + kAv1StreamWorkingSurfaces;
        damaged(stream);
        return;
    }
    if (!submit(stream, surface, unit)) {
        failure(stream);
        damaged(stream);
        return;
    }
    av1_stream_refresh_slots(state, surface);
    saturating_increment(state->decoded);
    state->waiting = false;
    stream->last_request_ms = 0; // A keyframe arrived or the stream is healthy.
    if (header.show_frame)
        present(stream, surface, header, rtp_timestamp);
}

void show_existing(GpuStream* stream, u32 rtp_timestamp) noexcept {
    Av1GpuStreamState* state = &stream->av1;
    const Av1FrameHeader& header = state->header;
    const u8 surface = state->slot_surface[header.frame_to_show_map_idx];
    if (state->waiting || surface == kAv1NoSurface) {
        saturating_increment(state->dropped);
        stream_request_keyframe(stream, state->unsupported);
        return;
    }
    // A shown key frame resets every slot to itself (spec 7.21).
    av1_stream_refresh_slots(state, surface);
    present(stream, surface, header, rtp_timestamp);
}

void reset(GpuStream* stream) noexcept {
    av1_stream_reset(&stream->av1);
}

void drain(GpuStream*) noexcept {
}

void decode(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact, bool resynchronize) noexcept {
    Av1GpuStreamState* state = &stream->av1;
    if (resynchronize)
        av1_stream_lose(state);
    state->frame_open = false;
    u32 offset = 0;
    bool frame_header_seen = false; // Later copies are redundant frame headers.
    while (offset < bytes.count) {
        Av1Obu obu{};
        if (av1_next_obu(bytes, &offset, &obu) != Av1Status::ok) {
            damaged(stream);
            return;
        }
        if (state->have_sequence && !av1_stream_in_operating_point(state->sequence, obu))
            continue;
        if (obu.type == kAv1ObuSequenceHeader) {
            Av1SequenceHeader sequence{};
            if (av1_parse_sequence_header(obu.payload, &sequence) != Av1Status::ok) {
                damaged(stream);
                return;
            }
            if (!state->have_sequence || !av1_stream_same_geometry(sequence, state->sequence)) {
                state->sequence_changed = true;
                state->unsupported = !decodable(sequence, driver(stream->device));
                av1_stream_lose(state); // A new sequence starts at a keyframe.
            }
            state->sequence = sequence;
            state->have_sequence = true;
            continue;
        }
        if (obu.type == kAv1ObuTemporalDelimiter) {
            frame_header_seen = false;
            continue;
        }
        const bool header = obu.type == kAv1ObuFrameHeader || obu.type == kAv1ObuFrame;
        if (!header && obu.type != kAv1ObuTileGroup)
            continue; // Metadata, padding, tile lists, redundant frame headers.
        if (obu.type == kAv1ObuFrameHeader && frame_header_seen)
            continue;
        if (!intact || !state->have_sequence || state->unsupported) {
            // Parameter sets above still apply; nothing is decoded until a keyframe.
            saturating_increment(intact ? state->dropped : state->damaged);
            av1_stream_lose(state);
            stream_request_keyframe(stream, state->unsupported);
            return;
        }
        ConstSlice<u8> group = obu.payload;
        if (header) {
            if (state->frame_open) {
                damaged(stream); // The previous frame lacks tiles.
                return;
            }
            const Av1Status status = av1_parse_frame_header(state->sequence, state->references, obu.payload,
                                                            obu.temporal_id, obu.spatial_id, &state->header);
            if (status != Av1Status::ok) {
                damaged(stream);
                return;
            }
            if (state->header.show_existing_frame) {
                show_existing(stream, rtp_timestamp);
                continue;
            }
            frame_header_seen = true;
            state->frame_open = true;
            state->tile_count = 0;
            if (obu.type == kAv1ObuFrameHeader)
                continue;
            group = {obu.payload.data + state->header.header_bytes, obu.payload.count - state->header.header_bytes};
        } else if (!state->frame_open) {
            damaged(stream);
            return;
        }
        bool complete = false;
        if (!av1_stream_gather_tiles(state, bytes, group, &complete)) {
            damaged(stream);
            return;
        }
        if (complete) {
            frame_header_seen = false;
            decode_frame(stream, bytes, rtp_timestamp);
        }
    }
    if (state->frame_open)
        damaged(stream);
}

void stats(const GpuStream& stream, GpuStreamDecoderStats* out) noexcept {
    const Av1GpuStreamState& state = stream.av1;
    out->decoded = state.decoded;
    out->dropped = state.dropped;
    out->damaged = state.damaged;
    out->unsupported = state.unsupported;
}

} // namespace av1

// Indexed by GpuStreamCodec.
const StreamBackend kBackends[]{{true, h264::reset, h264::decode, h264::drain, release_hardware, h264::stats},
                                {true, hevc::reset, hevc::decode, hevc::drain, release_hardware, hevc::stats},
                                {true, av1::reset, av1::decode, av1::drain, release_hardware, av1::stats}};

} // namespace

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
