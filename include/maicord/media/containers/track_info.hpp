#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// Owned UTF-8 metadata; no borrowed page, URL, token or stream lifetime.
struct MediaTrackId {
    union { u64 numeric{}; char youtube[11]; };
    bool youtube_track{}, apple_track{};
};
inline bool media_track_id_equal(const MediaTrackId& a, const MediaTrackId& b) noexcept {
    if (a.youtube_track != b.youtube_track || a.apple_track != b.apple_track) return false;
    if (!a.youtube_track) return a.numeric == b.numeric;
    for (u32 i = 0; i < 11; ++i) if (a.youtube[i] != b.youtube[i]) return false;
    return true;
}
inline constexpr u32 kMediaTrackTitleBytes = 512;
struct MediaTrackTitle {
    char text[kMediaTrackTitleBytes]{};
    u16 bytes{};
};
// Metadata callbacks borrow one decoded title only for the duration of write.
struct MediaTrackTitleSink { void* context{}; bool (*write)(void*, u16, const MediaTrackTitle&) noexcept{}; };
struct SoundcloudTitleBatch {
    struct Track { u64 id{}; u16 index{}; };
    char client[32]{};
    u16 count{};
    Track tracks[50]{};
};

}
