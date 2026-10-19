#pragma once

#include "maicord/media/av1.hpp"
#include "maicord/media/gpu_video_frame.hpp"

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

void av1_gpu_stream_reset(Av1GpuStreamState* state) noexcept;
// One temporal unit (OBUs, the last one possibly without obu_size).
void av1_gpu_stream_decode(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact,
                           bool resynchronize) noexcept;
void av1_gpu_stream_drain(GpuStream* stream) noexcept;
void av1_gpu_stream_stats(const GpuStream& stream, GpuStreamDecoderStats* stats) noexcept;

} // namespace maicord
