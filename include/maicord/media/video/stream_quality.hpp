#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// The quality a screen share is sent at: Discord's presets, the resolutions and frame rates the stream settings offer,
// which of them the sender's Nitro (or the server's boosts) unlock, and the encode extent and bitrate bounds each one
// means. Pure, so the settings UI, the session and tests share one table.
//
// A resolution is a picture height (480, 720, 1080, 1440) or 0, the source's own size. premium_type is the user
// object's (0 none, 1 Nitro Classic, 2 Nitro, 3 Nitro Basic); guild_premium_tier the server's boost tier (0 to 3; 0 in
// a call outside a server).

// Discord's values: "Smoother Video", "Clearer Text" and "Custom".
enum class StreamPreset : u8 { video = 1, text = 2, custom = 3 };

struct StreamQuality {
    u16 resolution;
    u8 fps;
};

// What the settings list, in their order. 5 fps is never listed: only the text preset falls to it.
inline constexpr u16 kStreamResolutions[]{480, 720, 1080, 1440, 0};
inline constexpr u8 kStreamFrameRates[]{15, 30, 60};
// Where every preset falls back to (and what anyone may send).
inline constexpr StreamQuality kStreamDefaultQuality{720, 30};

namespace stream_quality_detail {
// mid: Nitro Classic or Nitro; high: Nitro. boost: the server boost tier that also unlocks the entry (0: none does).
enum Perk : u8 { anyone, mid, high };
struct Entry {
    u16 resolution;
    u8 fps;
    Perk perk;
    u8 boost;
    bool text_only;
};
inline constexpr Entry kEntries[]{
    {0, 60, high, 0, false},    {0, 30, high, 0, false},    {0, 15, high, 0, false},    {0, 5, anyone, 0, true},
    {1440, 60, mid, 2, false},  {1440, 30, mid, 2, false},  {1440, 15, mid, 2, false},  {1080, 60, mid, 2, false},
    {1080, 30, mid, 2, false},  {1080, 15, mid, 2, false},  {720, 60, mid, 1, false},   {720, 30, anyone, 0, false},
    {720, 15, anyone, 0, false}, {720, 5, anyone, 0, false}, {480, 60, mid, 1, false},   {480, 30, anyone, 0, false},
    {480, 15, anyone, 0, false}, {480, 5, anyone, 0, false},
};
// What each preset tries, best first.
inline constexpr StreamQuality kVideoLadder[]{{1440, 60}, {1080, 60}, {720, 60}, {720, 30}};
inline constexpr StreamQuality kTextLadder[]{{0, 15}, {0, 5}};
} // namespace stream_quality_detail

constexpr bool stream_quality_allowed(StreamPreset preset, StreamQuality quality, u8 premium_type,
                                      u8 guild_premium_tier) noexcept {
    using namespace stream_quality_detail;
    for (const Entry& entry : kEntries) {
        if (entry.resolution != quality.resolution || entry.fps != quality.fps ||
            (entry.text_only && preset != StreamPreset::text))
            continue;
        const bool perk = entry.perk == anyone || (entry.perk == mid && (premium_type == 1 || premium_type == 2)) ||
                          (entry.perk == high && premium_type == 2);
        return perk || (entry.boost && guild_premium_tier >= entry.boost);
    }
    return false;
}

// The quality a preset streams at: its best allowed rung, the custom choice when that is allowed, else the default.
constexpr StreamQuality stream_preset_quality(StreamPreset preset, StreamQuality custom, u8 premium_type,
                                              u8 guild_premium_tier) noexcept {
    using namespace stream_quality_detail;
    if (preset == StreamPreset::video)
        for (const StreamQuality& rung : kVideoLadder)
            if (stream_quality_allowed(preset, rung, premium_type, guild_premium_tier))
                return rung;
    if (preset == StreamPreset::text)
        for (const StreamQuality& rung : kTextLadder)
            if (stream_quality_allowed(preset, rung, premium_type, guild_premium_tier))
                return rung;
    if (preset == StreamPreset::custom && stream_quality_allowed(preset, custom, premium_type, guild_premium_tier))
        return custom;
    return kStreamDefaultQuality;
}

struct StreamExtent {
    u32 width, height;
};

// The encoded picture for a source of source_width x source_height: the source scaled to fit the resolution's box
// (4:3 up to 480, else 16:9: 640x480, 1280x720, 1920x1080, 2560x1440) with its aspect kept, never enlarged, both sides
// even; resolution 0 keeps the source's size (made even). Sources past 4096 x 2304 pixels, the largest picture a
// stream's decoders take, fit into that. {0, 0} for an empty source.
constexpr StreamExtent stream_encode_extent(u16 resolution, u32 source_width, u32 source_height) noexcept {
    if (!source_width || !source_height)
        return {0, 0};
    u64 box_width = 4096, box_height = 2304;
    if (resolution) {
        box_height = resolution;
        box_width = resolution <= 480 ? resolution / 3u * 4u : resolution / 9u * 16u;
    }
    u64 width = source_width, height = source_height;
    if (width > box_width || height > box_height) {
        // Scale by the tighter of the two ratios, compared without division.
        if (width * box_height >= height * box_width) {
            height = (height * box_width + width / 2) / width;
            width = box_width;
        } else {
            width = (width * box_height + height / 2) / height;
            height = box_height;
        }
    }
    width &= ~u64{1};
    height &= ~u64{1};
    return {static_cast<u32>(width ? width : 2), static_cast<u32>(height ? height : 2)};
}

// Bits per second the sender adapts within (bandwidth estimation moves between minimum and maximum) and starts at.
struct StreamBitrate {
    u32 minimum, maximum, start;
};
constexpr StreamBitrate stream_quality_bitrate(StreamQuality quality) noexcept {
    const bool high = (quality.resolution == 0 && quality.fps >= 10) || quality.resolution > 720 || quality.fps > 30;
    return {500'000, high ? 9'000'000u : 3'500'000u, 600'000};
}

} // namespace maicord
