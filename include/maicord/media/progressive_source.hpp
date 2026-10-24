#pragma once

#include "maicord/media/mp4_demux.hpp"

namespace maicord {

// Sparse progressive MP4 source fed by HTTP Range responses. One address
// reservation spans the whole entity (<= kMediaMaximumProgressiveVideoBytes);
// fixed blocks are committed as whole ranges arrive and decommitted again when
// they fall outside every playback cursor's window.
//
// Memory bound, committed bytes per source, independent of entity size:
//   pinned metadata (top-level box headers and moov) <= kProgressiveMaximumMetadataBytes (16 MiB)
//   + media payload                                <= kProgressiveResidentBudgetBytes (40 MiB).
// The payload budget holds the read-ahead window (8..32 MiB, ~20 s at the
// average bitrate) plus kProgressiveBehindBytes kept behind each of the video
// and audio cursors. An idle lane trims back to pinned metadata only.
inline constexpr u32 kProgressiveBlockBytes = 256u * 1024u;
inline constexpr u32 kProgressiveMaximumBlocks = kMediaMaximumProgressiveVideoBytes / kProgressiveBlockBytes;
inline constexpr u32 kProgressiveHeadProbeBytes = 1024u * 1024u;
inline constexpr u32 kProgressiveRequestBytes = 2u * 1024u * 1024u;
inline constexpr u32 kProgressiveMetadataRequestBytes = 8u * 1024u * 1024u;
inline constexpr u32 kProgressiveMinimumAheadBytes = 8u * 1024u * 1024u;
inline constexpr u32 kProgressiveMaximumAheadBytes = 32u * 1024u * 1024u;
inline constexpr u32 kProgressiveAheadSeconds = 20;
inline constexpr u32 kProgressiveBehindBytes = 4u * 1024u * 1024u;
inline constexpr u32 kProgressiveResidentBudgetBytes = kProgressiveMaximumAheadBytes + 2 * kProgressiveBehindBytes;
inline constexpr u32 kProgressiveAudioResidentBytes = 4u * 1024u * 1024u;
inline constexpr u32 kProgressiveMaximumMetadataBytes = 16u * 1024u * 1024u;
inline constexpr u32 kProgressiveMaximumTopLevelBoxes = 64;

enum class ProgressiveTrack : u8 { video, audio };

struct ProgressiveSource;
// Availability context of one track's demuxer. Every query records the track's
// byte cursor; a refused query also records the first missing byte.
struct ProgressiveReader {
    ProgressiveSource* source;
    u64 cursor;
    u64 missing;
    bool pending;
    bool active;
    // Temporary current-fragment metadata, independently retained when its
    // media cursor moves far ahead. Unlike pinned init data, this is replaced.
    u64 metadata_first;
    u32 metadata_bytes;
    u64 seek_metadata_first;
    u32 seek_metadata_bytes;
};

struct ProgressiveSource {
    u8* base;
    u32 total_bytes;
    u32 block_count;
    u32 resident_blocks;
    u32 pinned_blocks;
    u32 ahead_bytes;
    bool audio_buffer;
    // Bumped by every write or eviction; consumers recompute derived views.
    u64 generation;
    u64 present[kProgressiveMaximumBlocks / 64];
    u64 pinned[kProgressiveMaximumBlocks / 64];
    ProgressiveReader readers[2];
};

struct ProgressiveRange {
    u64 first;
    u32 bytes;
};

// Reserves address space only. The source must not move while readers exist.
bool progressive_source_initialize(ProgressiveSource*, u32 total_bytes, bool audio_buffer = false) noexcept;
inline u32 progressive_source_budget(const ProgressiveSource& source) noexcept {
    return source.audio_buffer ? kProgressiveAudioResidentBytes : kProgressiveResidentBudgetBytes;
}
void progressive_source_release(ProgressiveSource*) noexcept;
ConstSlice<u8> progressive_source_bytes(const ProgressiveSource&) noexcept;
// Commits and copies every block the range fully covers (a short final block
// counts at the entity end). Partially covered blocks are left for a later
// block-aligned request. False only on commit failure or an invalid range.
bool progressive_source_write(ProgressiveSource*, u64 offset, ConstSlice<u8> bytes) noexcept;
bool progressive_source_available(const ProgressiveSource&, u64 offset, u64 bytes) noexcept;
bool progressive_source_complete(const ProgressiveSource&) noexcept;
u32 progressive_source_payload_blocks(const ProgressiveSource&) noexcept;
void progressive_source_pin(ProgressiveSource*, u64 offset, u64 bytes) noexcept;
// Read-ahead becomes ~kProgressiveAheadSeconds at the average bitrate.
void progressive_source_set_duration(ProgressiveSource*, u64 duration_us) noexcept;
Mp4SourceAvailability progressive_source_reader(ProgressiveSource*, ProgressiveTrack) noexcept;
// Current and optional pending-seek metadata leases per reader. New nonempty
// ranges must be resident; zero bytes releases the selected lease. No copies.
bool progressive_source_retain_metadata(ProgressiveSource*, ProgressiveTrack, u64 first, u32 bytes,
                                        bool seeking = false) noexcept;
void progressive_source_reset_readers(ProgressiveSource*) noexcept;
// Decommits unpinned blocks outside every active reader window until the
// payload plus one request fits budget_bytes: farthest behind first, then ahead.
u32 progressive_source_evict(ProgressiveSource*, u32 budget_bytes) noexcept;
// Drops every unpinned block except an outstanding reader metadata lease.
u32 progressive_source_trim(ProgressiveSource*) noexcept;

enum class ProgressiveProbeState : u8 { need_data, ready, unsupported, malformed };
struct ProgressiveProbe {
    ProgressiveRange need;
    ProgressiveProbeState state;
};
// Walks top-level boxes from the head, pinning each header and moov. Moov at
// the end is found from the mdat size; fragmented movies are unsupported.
ProgressiveProbe progressive_mp4_probe(ProgressiveSource*) noexcept;

struct ProgressivePlan {
    ProgressiveRange request;
    bool submit;
    bool cancel_inflight;
};
// One request at a time. Refused reads come first; with read_ahead, each active
// cursor's window is then filled. An in-flight range outside every demand
// window is stale and should be cancelled.
ProgressivePlan progressive_schedule(const ProgressiveSource&, ProgressiveRange inflight,
                                     bool read_ahead) noexcept;

// Buffered presentation ranges of the video track in 1/10000 of the duration,
// from a demuxer over this source. Adjacent runs merge; returns the count.
u32 progressive_buffered_ranges(const ProgressiveSource&, const Mp4Demuxer& video, u64 duration_us,
                                u16* begin, u16* end, u32 capacity) noexcept;

} // namespace maicord
