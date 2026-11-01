#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
enum class HlsPlaylistError : u8 { none, invalid_input, malformed, unsupported, capacity, stale, gap };
enum class HlsPlaylistMode : u8 { complete, sliding, complete_uri };
struct HlsVariant {
    Utf8View uri{};
    u32 bandwidth{};
    u16 width{}, height{};
};
struct HlsMasterPlaylist { HlsVariant video{}, audio{}; };
// Select direct AVC/AAC video and an independent AAC rendition from a bounded
// master playlist. AUDIO references select only their matching EXT-X-MEDIA
// group, preferring its default rendition. Borrowed URLs still require
// provider-specific admission. Two bounded passes, no rendition table.
HlsPlaylistError hls_master_playlist(Utf8View, u16 maximum_height, HlsMasterPlaylist*, u32 maximum_video_bandwidth = ~u32{0}, bool relative_uris = false) noexcept;
// Resolves a bounded HTTPS media reference against its playlist. Supports
// relative paths, dot segments and queries; rejects credentials, ports,
// fragments, scheme-relative URLs and traversal above the origin root.
// No network authorization: the caller must admit the resulting provider URL.
// Returns bytes written, or zero without touching output; no allocation.
u32 hls_resolve_uri(Utf8View base, Utf8View reference, Slice<char> output) noexcept;
struct HlsMediaPlaylist {
    Utf8View input{}, initialization{};
    u64 duration_us{}, sequence{}, discontinuity_sequence{};
    u32 segments{}, target_seconds{}, first_offset{}, maximum_duration_us{};
    HlsPlaylistMode mode{};
    bool ended{}, transport{};
};
struct HlsMediaSegment {
    Utf8View uri{};
    u64 time_us{};
    u32 duration_us{}, index{}, next_offset{};
};
static_assert(sizeof(HlsMediaPlaylist) <= 80 && sizeof(HlsMediaSegment) <= 48);
// Bounded clear, complete fMP4 VOD/ended-event playlist: one initialization, relative
// local paths, no encryption, byte ranges or discontinuities. At most 256 KiB,
// 8192 segments and 24 hours. Borrows immutable input; no allocation or copied
// URI/sample index. All outputs are transactional.
// complete_uri also accepts bounded relative/HTTPS references with query strings;
// resolve them against the playlist and admit the provider before fetching.
// Sliding mode allows unended snapshots and HTTPS media URIs, which still need
// provider admission. Each snapshot is bounded to 64 KiB and 128 segments.
// A sliding playlist without MAP selects MPEG-TS or timestamped packed AAC;
// each file must supply validated configuration. Format changes require resync.
HlsPlaylistError hls_media_playlist(Utf8View, HlsMediaPlaylist*, HlsPlaylistMode = HlsPlaylistMode::complete) noexcept;
struct HlsPlaylistAdvance { u64 discarded_us{}; u32 new_segments{}; bool changed{}; };
// Validate two parser-produced sliding snapshots before replacing borrowed text. Overlapping media
// must retain identity and duration; no hidden sequence gaps or clock resets.
// discarded_us advances the caller's window origin, never a decoder timestamp.
HlsPlaylistError hls_media_update(const HlsMediaPlaylist& previous, const HlsMediaPlaylist& next,
    HlsPlaylistAdvance*) noexcept;
bool hls_media_segment(const HlsMediaPlaylist&, u32 index, HlsMediaSegment*) noexcept;
// Sequential fetches resume from the previous line instead of rescanning.
bool hls_media_follow(const HlsMediaPlaylist&, const HlsMediaSegment&, HlsMediaSegment*) noexcept;
// EXTINF locates a file; its native container timestamps remain authoritative.
bool hls_media_find(const HlsMediaPlaylist&, u64 time_us, HlsMediaSegment*) noexcept;
} // namespace maicord
