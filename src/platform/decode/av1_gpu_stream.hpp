#pragma once

#include "maicord/media/video/av1.hpp"
#include "maicord/media/video/gpu_video_frame.hpp"

namespace maicord {

struct GpuStream;
struct GpuStreamDecoderStats;

// Tiles per frame: AV1 levels up to 6.3 allow 128.
inline constexpr u32 kAv1StreamTiles = 128;
// Surfaces beyond the distinct reference frames: the picture being decoded
// plus the presented and held slices. A stream starts with room for
// kAv1StreamInitialReferences distinct references (real-time encoders use
// one to three); one that needs more grows to all eight at its next keyframe.
inline constexpr u8 kAv1StreamInitialReferences = 3;
inline constexpr u8 kAv1StreamWorkingSurfaces = 1 + kVideoStreamPresentationSurfaces;

// AV1 backend state of a GpuStream (decode thread only).
struct Av1GpuStreamState {
    Av1SequenceHeader sequence;
    Av1References references;
    Av1FrameHeader header;
    Av1Tile tiles[kAv1StreamTiles]; // Gathered tiles of header's frame; offsets into the temporal unit.
    u8 slot_surface[kAv1RefFrames]; // Surface of each reference slot, 255 when none.
    u32 tile_count, decoded, dropped, damaged;
    u8 surfaces_wanted;
    bool have_sequence, sequence_changed, frame_open, waiting, unsupported;
};

// The codec-state halves of the AV1 backend (av1_gpu_stream.cpp, both systems). Once per frame: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
inline constexpr u8 kAv1NoSurface = 255;

inline void av1_stream_lose(Av1GpuStreamState* state) noexcept {
    state->waiting = true;
    state->frame_open = false;
}

// No reference slot holds a picture.
inline void av1_stream_clear_references(Av1GpuStreamState* state) noexcept {
    for (auto& reference : state->references)
        reference.valid = false;
    for (auto& surface : state->slot_surface)
        surface = kAv1NoSurface;
}

// The slots the header refreshes now hold surface.
inline void av1_stream_refresh_slots(Av1GpuStreamState* state, u8 surface) noexcept {
    av1_update_references(state->references, state->header);
    for (u32 i = 0; i < kAv1RefFrames; ++i)
        if (state->header.refresh_frame_flags >> i & 1)
            state->slot_surface[i] = surface;
}

// The sequence and pool size survive an idle release; references and the open frame do not.
inline void av1_stream_reset(Av1GpuStreamState* state) noexcept {
    av1_stream_clear_references(state);
    state->tile_count = 0;
    if (!state->surfaces_wanted)
        state->surfaces_wanted = kAv1StreamInitialReferences + kAv1StreamWorkingSurfaces;
    state->frame_open = false;
    state->waiting = true;
}

inline bool av1_stream_same_geometry(const Av1SequenceHeader& a, const Av1SequenceHeader& b) noexcept {
    return a.max_width == b.max_width && a.max_height == b.max_height && a.profile == b.profile &&
           a.bit_depth == b.bit_depth && a.mono_chrome == b.mono_chrome && a.subsampling_x == b.subsampling_x &&
           a.subsampling_y == b.subsampling_y && a.use_128x128_superblock == b.use_128x128_superblock;
}

// The first of `count` surfaces that is neither in `used` nor a reference slot's; kAv1NoSurface when none is.
inline u8 av1_stream_free_surface(const Av1GpuStreamState& state, u32 used, u32 count) noexcept {
    for (const u8 surface : state.slot_surface)
        if (surface != kAv1NoSurface)
            used |= 1u << surface;
    for (u32 i = 0; i < count; ++i)
        if (!(used >> i & 1))
            return static_cast<u8>(i);
    return kAv1NoSurface;
}

inline bool av1_stream_in_operating_point(const Av1SequenceHeader& sequence, const Av1Obu& obu) noexcept {
    const u32 idc = sequence.operating_point_idc;
    return !obu.extension || !idc || ((idc >> obu.temporal_id & 1) && (idc >> (obu.spatial_id + 8) & 1));
}

// Tiles of one tile group, offsets made relative to the temporal unit.
inline bool av1_stream_gather_tiles(Av1GpuStreamState* state, ConstSlice<u8> unit, ConstSlice<u8> group,
                                    bool* complete) noexcept {
    u32 start = 0, end = 0;
    const Av1Status status = av1_parse_tile_group(
        state->header, group, {state->tiles + state->tile_count, kAv1StreamTiles - state->tile_count}, &start, &end);
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
#pragma clang attribute pop

void av1_gpu_stream_reset(GpuStream* stream) noexcept;
// One temporal unit (OBUs, the last one possibly without obu_size).
void av1_gpu_stream_decode(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact,
                           bool resynchronize) noexcept;
void av1_gpu_stream_drain(GpuStream* stream) noexcept;
void av1_gpu_stream_stats(const GpuStream& stream, GpuStreamDecoderStats* stats) noexcept;
// The system half (gpu_stream_dxva.cpp, vaapi/va_stream.cpp), as for H.264 and HEVC in gpu_stream_decoder_backend.hpp:
// whether this GPU decodes the sequence, (re)creating the decoder for it, and one frame into surface.
bool av1_gpu_stream_decodable(const GpuStream& stream, const Av1SequenceHeader& sequence) noexcept;
bool av1_gpu_stream_create(GpuStream* stream) noexcept;
bool av1_gpu_stream_submit(GpuStream* stream, u8 surface, ConstSlice<u8> unit) noexcept;

} // namespace maicord
