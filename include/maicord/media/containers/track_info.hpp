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
// What the client picked to play one provider track. All zero: no pick to report.
// A YouTube pick is H.264 video with Opus or AAC audio, or the audio alone for an art track of YouTube Music. The
// client plays no other pick from YouTube.
enum class MediaQualityProvider : u8 { none, youtube, youtube_music };
struct MediaPlaybackQuality {
    // Bits per second: the mean bitrate of the format, or its peak without one. 0 when unknown.
    u32 video_bitrate, audio_bitrate;
    u16 width, height;
    u8 fps, channels;
    MediaQualityProvider provider;
    // The audio is Opus. It is AAC without this.
    bool opus;
    // How far the provider's loudness rule lowers the sound, in 1/100 dB. 0: the source plays at its own level.
    u16 attenuation;
};
// The picked track has no picture: the player shows a still picture and decodes no video.
constexpr bool media_quality_sound_only(const MediaPlaybackQuality& quality) noexcept {
    return quality.provider != MediaQualityProvider::none && !quality.width;
}
// The linear gain of an attenuation in 1/100 dB: 10^(-attenuation / 2000).
inline float media_attenuation_gain(u32 attenuation) noexcept {
    // 10^(-a / 2000) is 2^(-a * log2(10) / 2000). Each whole octave halves the gain. A series gives the rest.
    const double octaves = attenuation * 0.0016609640474436812;
    const u32 whole = static_cast<u32>(octaves);
    const double exponent = (octaves - whole) * -0.6931471805599453;
    double term = 1, sum = 1;
    for (u32 i = 1; i < 12; ++i) {
        term *= exponent / i;
        sum += term;
    }
    return whole < 63 ? static_cast<float>(sum / static_cast<double>(u64{1} << whole)) : 0.0f;
}

}
