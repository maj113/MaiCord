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
// A track's title, then optionally its author and its length in seconds, each after a 0x1F byte (a title never holds
// control bytes): "title\x1Fauthor\x1Fseconds"; either may be empty. `id` is the track a playlist lookup answered for.
inline constexpr char kMediaTrackDetail = '\x1F';
struct MediaTrackTitle {
    char text[kMediaTrackTitleBytes];
    u16 bytes;
    MediaTrackId id;
};
struct MediaTrackTitleParts {
    Utf8View title, author;
    u32 seconds;
};
inline MediaTrackTitleParts media_track_title_parts(Utf8View text) noexcept {
    MediaTrackTitleParts parts{};
    u32 part = 0, start = 0;
    for (u32 i = 0; i <= text.bytes; ++i) {
        if (i < text.bytes && text.data[i] != kMediaTrackDetail) {
            if (part == 2 && text.data[i] >= '0' && text.data[i] <= '9' && parts.seconds < 1'000'000)
                parts.seconds = parts.seconds * 10 + static_cast<u32>(text.data[i] - '0');
            continue;
        }
        if (part < 2)
            (part ? parts.author : parts.title) = {text.data + start, i - start};
        ++part;
        start = i + 1;
    }
    return parts;
}
// Metadata callbacks borrow one decoded title only for the duration of write.
struct MediaTrackTitleSink { void* context; bool (*write)(void*, u16, const MediaTrackTitle&) noexcept; };
struct SoundcloudTitleBatch {
    struct Track { u64 id; u16 index; };
    char client[32];
    u16 count;
    Track tracks[50];
};

}
