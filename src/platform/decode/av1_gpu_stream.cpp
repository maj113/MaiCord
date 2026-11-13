#include "av1_gpu_stream.hpp"
#include "gpu_stream_decoder_backend.hpp"

#include "maicord/core/memory.hpp"
// The AV1 backend of both systems over their submission (av1_gpu_stream_create, av1_gpu_stream_submit): once per
// frame and no byte loop, compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {

void damaged(GpuStream* stream) noexcept {
    Av1GpuStreamState* state = &stream->av1;
    saturating_increment(state->damaged);
    av1_stream_lose(state);
    stream_request_keyframe(stream, state->unsupported);
}

void present(GpuStream* stream, u8 surface, const Av1FrameHeader& header, u32 rtp_timestamp) noexcept {
    const Av1SequenceHeader& sequence = stream->av1.sequence;
    // render_size crops a coded frame that is larger than the picture.
    const u32 width = minimum(header.render_width, header.upscaled_width);
    const u32 height = minimum(header.render_height, header.frame_height);
    if (header.frame_type == av1_key_frame || stream->frame.width != width || stream->frame.height != height ||
        !stream->frame.texture)
        stream_set_format(stream, 0, 0, width, height, sequence.matrix_coefficients, sequence.color_range, 1, 1);
    if (!stream_show(stream, surface, rtp_timestamp, header.frame_type == av1_key_frame, &stream->av1.unsupported))
        damaged(stream);
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
#if !defined(_WIN32)
    // The VA decoder shows reference pictures without film grain: such a stream is the software tier's.
    if (header.film_grain.apply_grain && state->sequence.film_grain_params_present) {
        state->unsupported = true;
        av1_stream_lose(state);
        return;
    }
#endif
    if (header.frame_type == av1_key_frame &&
        (!stream_decoder_open(*stream) || state->sequence_changed || stream->surfaces.count < state->surfaces_wanted)) {
        const bool created = av1_gpu_stream_create(stream);
        av1_stream_clear_references(state);
        state->sequence_changed = false;
        if (!created) {
            if (state->unsupported)
                av1_stream_lose(state);
            else
                damaged(stream);
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
    if (!av1_gpu_stream_submit(stream, surface, unit)) {
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
} // namespace

void av1_gpu_stream_reset(GpuStream* stream) noexcept {
    av1_stream_reset(&stream->av1);
}

void av1_gpu_stream_decode(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact,
                           bool resynchronize) noexcept {
    Av1GpuStreamState* state = &stream->av1;
    if (resynchronize)
        av1_stream_lose(state);
#if defined(_WIN32)
    if (stream->stats.device_lost) { // Only DXVA reports a lost device.
        saturating_increment(state->dropped);
        return;
    }
#endif
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
                state->unsupported = !av1_gpu_stream_decodable(*stream, sequence);
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
            const Av1Status status =
                av1_parse_frame_header(state->sequence, state->references, obu.payload, obu.temporal_id,
                                       obu.spatial_id, &state->header);
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
            group = {obu.payload.data + state->header.header_bytes,
                     obu.payload.count - state->header.header_bytes};
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

void av1_gpu_stream_drain(GpuStream*) noexcept {
}

void av1_gpu_stream_stats(const GpuStream& stream, GpuStreamDecoderStats* stats) noexcept {
    const Av1GpuStreamState& state = stream.av1;
    stats->decoded = state.decoded;
    stats->dropped = state.dropped;
    stats->damaged = state.damaged;
    stats->unsupported = state.unsupported;
}

} // namespace maicord
#pragma clang attribute pop
