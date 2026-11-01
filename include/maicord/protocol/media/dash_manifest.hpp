#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
enum class DashManifestError : u8 { none, invalid_input, malformed, unsupported, capacity };
struct DashTrack {
    Utf8View id{}, initialization{}, media{};
    u64 segment_duration{};
    u32 timescale{}, start_number{1}, segment_count{}, bandwidth{};
    u16 width{}, height{};
};
struct DashManifest {
    DashTrack video{}, audio{};
    u64 duration_us{};
};
static_assert(sizeof(DashManifest) <= 192);
// Bounded clear static DASH profile: one Period, explicit fixed-duration
// SegmentTemplate per representation, relative paths, AVC and AAC-LC.
// No allocation/XML entity expansion, DRM, live refresh or inherited BaseURL.
// Views borrow the immutable manifest. Output is transactional. Manifest
// durations locate segments; actual SIDX/moof clocks remain authoritative.
DashManifestError dash_manifest_parse(Utf8View, u16 maximum_height, DashManifest*) noexcept;
// Only RepresentationID and Number (optionally %0Nd, N=1..10). Segment is a
// zero-based ordinal, not the manifest's startNumber. Never changes origins.
// Invalid input or short output leaves the output untouched.
u32 dash_segment_path(const DashTrack&, u32 segment, bool initialization, Slice<u8>) noexcept;
} // namespace maicord
