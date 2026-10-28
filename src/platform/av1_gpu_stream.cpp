#include "av1_gpu_stream.hpp"
#include "av1_dxva_parameters.hpp"
#include "gpu_stream_decoder_backend.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {

constexpr u8 kNoSurface = 255;

void lose(Av1GpuStreamState* state) noexcept {
    state->waiting = true;
    state->frame_open = false;
}

// No reference slot holds a picture.
void clear_references(Av1GpuStreamState* state) noexcept {
    for (auto& reference : state->references)
        reference.valid = false;
    for (auto& surface : state->slot_surface)
        surface = kNoSurface;
}

// The slots header refreshes now hold surface.
void refresh_slots(Av1GpuStreamState* state, u8 surface) noexcept {
    av1_update_references(state->references, state->header);
    for (u32 i = 0; i < kAv1RefFrames; ++i)
        if (state->header.refresh_frame_flags >> i & 1)
            state->slot_surface[i] = surface;
}

void damaged(GpuStream* stream) noexcept {
    Av1GpuStreamState* state = &stream->av1;
    saturating_increment(state->damaged);
    lose(state);
    gpu_stream_request_keyframe(stream, state->unsupported);
}

// Hardware decode here is 8-bit 4:2:0 into NV12; P010 is not negotiated.
bool decodable(const Av1SequenceHeader& sequence) noexcept {
    return sequence.profile == 0 && sequence.bit_depth == 8 && !sequence.mono_chrome &&
           sequence.subsampling_x && sequence.subsampling_y && sequence.max_width <= 4096 &&
           sequence.max_height <= 4096;
}

bool same_geometry(const Av1SequenceHeader& a, const Av1SequenceHeader& b) noexcept {
    return a.max_width == b.max_width && a.max_height == b.max_height && a.profile == b.profile &&
           a.bit_depth == b.bit_depth && a.mono_chrome == b.mono_chrome &&
           a.subsampling_x == b.subsampling_x && a.subsampling_y == b.subsampling_y &&
           a.use_128x128_superblock == b.use_128x128_superblock;
}

HRESULT create_decoder(GpuStream* stream) noexcept {
    Av1GpuStreamState* state = &stream->av1;
    const Av1SequenceHeader& sequence = state->sequence;
    // Surfaces cover whole superblocks of the largest frame.
    const u32 align = sequence.use_128x128_superblock ? 127u : 63u;
    GpuStreamDecoderDesc desc{};
    desc.profile = &D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0;
    desc.width = sequence.max_width;
    desc.height = sequence.max_height;
    desc.surface_width = (sequence.max_width + align) & ~align;
    desc.surface_height = (sequence.max_height + align) & ~align;
    desc.rate = {60, 1};
    desc.bitstream_raw = 1;
    desc.surfaces = state->surfaces_wanted;
    const HRESULT status = gpu_stream_create_decoder(stream, desc);
    clear_references(state);
    state->sequence_changed = false;
    return status;
}

u8 free_surface(const GpuStream& stream) noexcept {
    u32 used = gpu_stream_held(stream);
    for (const u8 surface : stream.av1.slot_surface)
        if (surface != kNoSurface)
            used |= 1u << surface;
    for (u8 i = 0; i < stream.surfaces.count; ++i)
        if (!(used >> i & 1))
            return i;
    return kNoSurface;
}

// One frame into surface. A batch's bitstream is the temporal unit span from
// its first to last tile, zero-padded to 128 bytes with at least one zero.
HRESULT submit(GpuStream* stream, u8 surface, const DXVA_PicParams_AV1& parameters,
               ConstSlice<u8> unit) noexcept {
    struct Source {
        const u8* unit;
        const Av1Tile* tiles;
    } const source{unit.data, stream->av1.tiles};
    const auto fill = [](const void* opaque, u32 first, u8* bytes, u32 capacity, void* control, u32 limit,
                         u32* written) noexcept -> u32 {
        const auto& from = *static_cast<const Source*>(opaque);
        auto* entries = static_cast<DXVA_Tile_AV1*>(control);
        const u32 start = from.tiles[first].offset;
        u32 batch = 0, end = start;
        for (; batch < limit; ++batch) {
            const Av1Tile& tile = from.tiles[first + batch];
            const u64 tile_end = u64{tile.offset} + tile.size;
            if (tile.offset < start || ((tile_end - start + 128u) & ~127ull) > capacity)
                break;
            entries[batch] = {tile.offset - start, tile.size, tile.row, tile.column, 0, 0xff, 0};
            end = maximum(end, static_cast<u32>(tile_end));
        }
        if (batch) {
            *written = static_cast<u32>((u64{end} - start + 128u) & ~127ull);
            copy_bytes(bytes, from.unit + start, end - start);
            clear_bytes(bytes + (end - start), *written - (end - start));
        }
        return batch;
    };
    return gpu_stream_submit(stream, {surface,
                                      {reinterpret_cast<const u8*>(&parameters), sizeof(parameters)},
                                      {},
                                      stream->av1.tile_count,
                                      u32{sizeof(DXVA_Tile_AV1)},
                                      kAv1StreamTiles,
                                      &source,
                                      fill});
}

void present(GpuStream* stream, u8 surface, const Av1FrameHeader& header, u32 rtp_timestamp) noexcept {
    const Av1SequenceHeader& sequence = stream->av1.sequence;
    // render_size crops a coded frame that is larger than the picture.
    const u32 width = minimum(header.render_width, header.upscaled_width);
    const u32 height = minimum(header.render_height, header.frame_height);
    if (header.frame_type == av1_key_frame || stream->frame.width != width ||
        stream->frame.height != height || !stream->frame.texture)
        gpu_stream_set_format(stream, 0, 0, width, height, sequence.matrix_coefficients,
                              sequence.color_range, 1, 1);
    gpu_stream_present(stream, surface, rtp_timestamp, header.frame_type == av1_key_frame);
}

void decode_frame(GpuStream* stream, ConstSlice<u8> unit, u32 rtp_timestamp) noexcept {
    Av1GpuStreamState* state = &stream->av1;
    const Av1FrameHeader& header = state->header;
    state->frame_open = false;
    if (state->waiting && header.frame_type != av1_key_frame) {
        saturating_increment(state->dropped);
        gpu_stream_request_keyframe(stream, state->unsupported);
        return;
    }
    if (header.frame_type == av1_key_frame &&
        (!stream->decoder || state->sequence_changed || stream->surfaces.count < state->surfaces_wanted)) {
        const HRESULT status = create_decoder(stream);
        if (FAILED(status)) {
            if (status == DXGI_ERROR_UNSUPPORTED || status == E_INVALIDARG) {
                state->unsupported = true;
                stream->stats.last_status = status;
                lose(state);
            } else {
                gpu_stream_failure(stream, status);
                damaged(stream);
            }
            return;
        }
        saturating_increment(stream->stats.reinitializations);
    } else if (state->waiting) {
        // Decoding restarts at this keyframe: earlier slots are not trusted.
        clear_references(state);
    }
    const u8 surface = free_surface(*stream);
    if (surface == kNoSurface) {
        // More distinct references than the pool holds: grow at the next keyframe.
        state->surfaces_wanted = kAv1RefFrames + kAv1StreamWorkingSurfaces;
        damaged(stream);
        return;
    }
    DXVA_PicParams_AV1 parameters;
    av1_dxva_picture_parameters(state->sequence, header, state->slot_surface, state->references, surface,
                                &parameters);
    const HRESULT status = submit(stream, surface, parameters, unit);
    if (FAILED(status)) {
        gpu_stream_failure(stream, status);
        damaged(stream);
        return;
    }
    refresh_slots(state, surface);
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
    if (state->waiting || surface == kNoSurface) {
        saturating_increment(state->dropped);
        gpu_stream_request_keyframe(stream, state->unsupported);
        return;
    }
    // A shown key frame resets every slot to itself (spec 7.21).
    refresh_slots(state, surface);
    present(stream, surface, header, rtp_timestamp);
}

bool in_operating_point(const Av1SequenceHeader& sequence, const Av1Obu& obu) noexcept {
    const u32 idc = sequence.operating_point_idc;
    return !obu.extension || !idc || ((idc >> obu.temporal_id & 1) && (idc >> (obu.spatial_id + 8) & 1));
}

// Tiles of one tile group, offsets made relative to the temporal unit.
bool gather_tiles(Av1GpuStreamState* state, ConstSlice<u8> unit, ConstSlice<u8> group,
                  bool* complete) noexcept {
    u32 start = 0, end = 0;
    const Av1Status status = av1_parse_tile_group(
        state->header, group, {state->tiles + state->tile_count, kAv1StreamTiles - state->tile_count},
        &start, &end);
    if (status == Av1Status::unsupported)
        state->unsupported = true;
    if (status != Av1Status::ok || start != state->tile_count)
        return false;
    const u32 base = static_cast<u32>(group.data - unit.data);
    for (u32 i = start; i <= end; ++i)
        state->tiles[i].offset += base;
    state->tile_count = end + 1;
    *complete = state->tile_count == u32{state->header.tile_columns} * state->header.tile_rows;
    return true;
}

} // namespace

void av1_gpu_stream_reset(GpuStream* stream) noexcept {
    // The sequence header and pool size survive an idle release.
    Av1GpuStreamState* state = &stream->av1;
    clear_references(state);
    state->tile_count = 0;
    if (!state->surfaces_wanted)
        state->surfaces_wanted = kAv1StreamInitialReferences + kAv1StreamWorkingSurfaces;
    state->frame_open = false;
    state->waiting = true;
}

void av1_gpu_stream_decode(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact,
                           bool resynchronize) noexcept {
    Av1GpuStreamState* state = &stream->av1;
    if (resynchronize)
        lose(state);
    if (stream->stats.device_lost) {
        saturating_increment(state->dropped);
        return;
    }
    state->frame_open = false;
    u32 offset = 0;
    bool frame_header_seen = false; // Later copies are redundant frame headers.
    while (offset < bytes.count) {
        Av1Obu obu{};
        if (av1_next_obu(bytes, &offset, &obu) != Av1Status::ok) {
            damaged(stream);
            return;
        }
        if (state->have_sequence && !in_operating_point(state->sequence, obu))
            continue;
        if (obu.type == kAv1ObuSequenceHeader) {
            Av1SequenceHeader sequence{};
            if (av1_parse_sequence_header(obu.payload, &sequence) != Av1Status::ok) {
                damaged(stream);
                return;
            }
            if (!state->have_sequence || !same_geometry(sequence, state->sequence)) {
                state->sequence_changed = true;
                state->unsupported = !decodable(sequence);
                lose(state); // A new sequence starts at a keyframe.
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
            lose(state);
            gpu_stream_request_keyframe(stream, state->unsupported);
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
        if (!gather_tiles(state, bytes, group, &complete)) {
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
