#pragma once
#include "maicord/media/mp4_demux.hpp"

namespace maicord {
struct Mp4AudioDemuxer {
    Mp4Demuxer samples;
    u32 sample_rate{}, asc_offset{}, asc_bytes{};
    u8 channels{};
    u16 frames_per_packet{};
};
static_assert(sizeof(Mp4AudioDemuxer) == sizeof(Mp4Demuxer) + 16);
// One internal mono/stereo AAC-LC or explicit HE-AAC mp4a track. sample_rate
// and frames_per_packet describe post-SBR/PS PCM, not the core AAC clock.
// ASC and packets are borrowed source slices. Unsupported 960-frame mode,
// dependency/complex edits and fragmented/external tracks fail.
Mp4DemuxError mp4_audio_demux_initialize(Mp4AudioDemuxer*, ConstSlice<u8>, Mp4SourceAvailability = {}) noexcept;
ConstSlice<u8> mp4_audio_demux_asc(const Mp4AudioDemuxer&) noexcept;
Mp4AccessUnit mp4_audio_demux_next(Mp4AudioDemuxer*) noexcept;
void mp4_audio_demux_reset(Mp4AudioDemuxer*) noexcept;
struct Mp4AudioSampleWindow {
    u64 first_source_frame{}, end_source_frame{}, leading_silence_frames{};
    i64 duration_100ns{};
};
// Exact original timebase/edit rational arithmetic, with only the final native
// timestamp rounded DOWN to 100ns. Source frame includes the endpoint sentinel;
// no decoder priming/delay is inferred. Failure leaves caller output unchanged.
bool mp4_audio_demux_frame_time_100ns(const Mp4AudioDemuxer&, u64 source_frame, i64*) noexcept;
struct Mp4AudioFrameTimeBounds {
    i64 floor_100ns{}, ceil_100ns{};
};
// Bounds are derived from the same rational source/edit time. They
// coincide for integer-exact timestamps; no blanket one-tick tolerance.
// Failure, overflow, and aliased output leave the caller value unchanged.
bool mp4_audio_demux_frame_time_bounds_100ns(const Mp4AudioDemuxer&, u64 source_frame,
                                             Mp4AudioFrameTimeBounds*) noexcept;
// Keep source samples in [first,end). Boundaries use original rational edits,
// not rounded us/100ns. Leading empty-edit gap is rounded UP to the
// source sample grid. Caller validates native PCM timestamp/frame identity.
bool mp4_audio_demux_sample_window(const Mp4AudioDemuxer&, i64 target_100ns,
                                   Mp4AudioSampleWindow*) noexcept;
} // namespace maicord
