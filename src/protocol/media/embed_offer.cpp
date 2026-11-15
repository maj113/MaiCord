#include "maicord/protocol/media/embed_player.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/log_event.hpp"
#include "maicord/core/name_list.hpp"

namespace maicord {
namespace {
// In the order of EmbedCodec and of EmbedFormatReason.
constexpr char kCodecs[] = "other\0h264\0vp9\0av1\0aac\0he-aac\0opus\0";
constexpr char kReasons[] = "none\0protected\0container\0codec\0size\0length\0channels\0track\0bitrate\0";
// The summary's keys: six places for video, then five for audio.
constexpr char kPlaces[] = "video_pick\0video_playable_best\0video_best\0h264_best\0vp9_best\0av1_best\0"
                           "audio_pick\0audio_playable_best\0audio_best\0aac_best\0opus_best\0";
constexpr u32 kVideoPlaces = 6, kPlaceCount = 11;

bool is_audio(const EmbedFormat& format) noexcept {
    return (format.flags & embed_format_audio) != 0;
}
// Orders two video formats or two audio formats. The size of a picture is its shorter side. Audio has no size and no
// frame rate, so its bitrate decides.
bool better(const EmbedFormat& a, const EmbedFormat& b) noexcept {
    const u32 size = minimum(a.width, a.height), old_size = minimum(b.width, b.height);
    return size != old_size ? size > old_size : a.fps != b.fps ? a.fps > b.fps : a.bitrate > b.bitrate;
}
struct Text {
    char data[64];
    u32 bytes;
    void put(const char* text) noexcept {
        while (*text && bytes + 1 < sizeof(data))
            data[bytes++] = *text++;
        data[bytes] = 0;
    }
    void number(u32 value, const char* unit) noexcept {
        char digits[11];
        digits[decimal_write(digits, value)] = 0;
        put(digits);
        put(unit);
    }
    // Hundredths as "-4.64".
    void hundredths(i32 value) noexcept {
        const u32 magnitude = static_cast<u32>(value < 0 ? -value : value);
        put(value < 0 ? "-" : "");
        number(magnitude / 100, magnitude % 100 < 10 ? ".0" : ".");
        number(magnitude % 100, "");
    }
};
// "h264 1920x1080 60 4231kbps" or "aac 129kbps 44100Hz 2ch". An HDR format ends with " hdr".
void describe(const EmbedFormat& format, Text* text) noexcept {
    text->put(name_list_at(kCodecs, static_cast<u32>(format.codec)).data);
    text->put(" ");
    if (!is_audio(format)) {
        text->number(format.width, "x");
        text->number(format.height, " ");
        text->number(format.fps, " ");
    }
    text->number((format.bitrate + 500) / 1000, "kbps");
    if (is_audio(format)) {
        text->put(" ");
        text->number(format.sample_rate, "Hz ");
        text->number(format.channels, "ch");
    }
    if (format.flags & embed_format_hdr)
        text->put(" hdr");
}
// The format whose sound the client plays: the picked audio format, or the picked combined format.
const EmbedFormat* sound_pick(const EmbedOffer& offer) noexcept {
    const EmbedFormat* combined = nullptr;
    for (u32 i = 0; i < offer.count; ++i) {
        const auto& format = offer.formats[i];
        if (!(format.flags & embed_format_picked))
            continue;
        if (is_audio(format))
            return &format;
        if (format.flags & embed_format_combined)
            combined = &format;
    }
    return combined;
}
// YouTube's loudness rule, as its web players apply it. The gain is the target minus the content loudness, and it is
// never above 0 dB. The target is -7 LKFS on music.youtube.com and -14 LKFS on youtube.com. A format with dynamic
// range compression gets no gain. A format without a loudness value gets no gain. The result is the attenuation in
// 1/100 dB.
u32 attenuation(const EmbedFormat& format, bool music) noexcept {
    if ((format.flags & (embed_format_loudness | embed_format_drc)) != embed_format_loudness)
        return 0;
    const i32 over = format.loudness - (music ? -700 : -1400);
    return over > 0 ? static_cast<u32>(over) : 0;
}
} // namespace

void embed_offer_summary(const EmbedOffer& offer, bool music, LogEvent* line) noexcept {
    // In the order of kPlaces. A place stays null when no format fills it, and its key stays out of the line.
    const EmbedFormat* best[kPlaceCount]{};
    u32 playable = 0;
    for (u32 i = 0; i < offer.count; ++i) {
        const auto& format = offer.formats[i];
        const u32 base = is_audio(format) ? kVideoPlaces : 0;
        const bool can = format.reason == EmbedFormatReason::none;
        playable += can;
        if (format.flags & embed_format_picked)
            best[base] = &format;
        if (format.flags & (embed_format_drc | embed_format_dubbed))
            continue;
        // After the pick come the best playable format and the best format. Then comes the codec's own place: h264,
        // vp9 and av1 for video, aac and opus for audio. Place 0 here means "none".
        static constexpr u8 kCodecPlace[]{0, 3, 4, 5, 3, 0, 4}; // in the order of EmbedCodec
        const u32 places[]{can ? 1u : 0u, 2u, kCodecPlace[static_cast<u32>(format.codec)]};
        for (const u32 place : places)
            if (place && (!best[base + place] || better(format, *best[base + place])))
                best[base + place] = &format;
    }
    line->field("provider", music ? "youtube_music" : "youtube");
    NameList key{kPlaces};
    for (const auto* format : best) {
        if (format) {
            Text text{};
            describe(*format, &text);
            line->quoted(key.at, text.data);
        }
        key.next();
    }
    if (const auto* sound = sound_pick(offer); sound && (sound->flags & embed_format_loudness)) {
        Text loudness{}, gain{};
        loudness.hundredths(sound->loudness);
        gain.hundredths(-static_cast<i32>(attenuation(*sound, music)));
        line->field("loudness_lkfs", loudness.data).field("target_lkfs", music ? "-7" : "-14").field("gain_db", gain.data);
    }
    const u64 counts[]{offer.count, playable};
    line->fields("formats\0playable", counts);
}

void embed_offer_format(const EmbedFormat& format, LogEvent* line) noexcept {
    const bool can = format.reason == EmbedFormatReason::none;
    line->field("itag", format.itag)
        .field("codec", name_list_at(kCodecs, static_cast<u32>(format.codec)).data)
        .field("container", name_list_at("other\0mp4\0webm\0", static_cast<u32>(format.container)).data);
    const u64 sound[]{format.sample_rate, format.channels}, picture[]{format.width, format.height, format.fps};
    if (is_audio(format))
        line->fields("sample_rate\0channels", sound);
    else
        line->fields("width\0height\0fps", picture);
    Text flags{};
    flags.put(format.flags & embed_format_combined ? "combined" : "adaptive");
    // In the order of the flag bits from embed_format_hdr.
    NameList name{"hdr\0drc\0original\0dubbed\0"};
    for (u32 bit = embed_format_hdr; name; bit <<= 1, name.next())
        if (format.flags & bit) {
            flags.put(",");
            flags.put(name.at);
        }
    const u64 state[]{can, (format.flags & embed_format_picked) != 0};
    line->field("bitrate", format.bitrate).field("flags", flags.data).fields("playable\0picked", state);
    if (!can)
        line->field("reason", name_list_at(kReasons, static_cast<u32>(format.reason)).data);
}

MediaPlaybackQuality embed_offer_quality(const EmbedOffer& offer, bool music) noexcept {
    MediaPlaybackQuality quality{};
    if (const auto* sound = sound_pick(offer))
        quality.attenuation = static_cast<u16>(attenuation(*sound, music));
    for (u32 i = 0; i < offer.count; ++i) {
        const auto& format = offer.formats[i];
        if (!(format.flags & embed_format_picked))
            continue;
        quality.provider = music ? MediaQualityProvider::youtube_music : MediaQualityProvider::youtube;
        if (is_audio(format)) {
            quality.audio_bitrate = format.bitrate;
            quality.channels = format.channels;
            quality.opus = format.codec == EmbedCodec::opus;
            continue;
        }
        quality.video_bitrate = format.bitrate;
        quality.width = format.width;
        quality.height = format.height;
        quality.fps = format.fps;
    }
    return quality;
}
} // namespace maicord
