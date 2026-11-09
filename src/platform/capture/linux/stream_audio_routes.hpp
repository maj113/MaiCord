#pragma once

// Which PipeWire playback streams stream audio records, and onto which of its two inputs: the rules of
// src/platform/capture/linux/stream_audio.cpp, kept apart so tests/platform/capture/capture.cpp checks them directly.

#include "maicord/core/slice.hpp"
#include "maicord/platform/audio/stream_audio.hpp"

namespace maicord {

// The decimal value of a property, 0 when it is missing or not a number.
constexpr u32 stream_audio_property_number(const char* text) noexcept {
    if (!text || !*text)
        return 0;
    u64 value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || value > 0xFFFFFFFFull / 10)
            return 0;
        value = value * 10 + static_cast<u32>(*text - '0');
    }
    return value > 0xFFFFFFFFull ? 0 : static_cast<u32>(value);
}

constexpr bool stream_audio_text_equal(const char* a, const char* b) noexcept {
    if (!a || !b)
        return false;
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

// A node whose output is recorded: an application's playback stream (media.class Stream/Output/Audio: `playback`) whose
// process (application.process.id of the node, else of its client; 0 when neither says) is not MaiCord's
// `own_process` (MaiCord's voice, media and notification streams alike); for an application scope with a process id,
// only that process's streams.
constexpr bool stream_audio_node_recorded(bool playback, u32 process, u32 own_process,
                                          const StreamAudioOptions& options) noexcept {
    if (!playback || process == own_process)
        return false;
    return options.scope != StreamAudioScope::application || !options.process_id || process == options.process_id;
}

// The inputs (bit 0 left, bit 1 right) an output port of `channel` (audio.channel) is linked to: left and right
// channels to their side, mono and centre to both, anything else (LFE, unknown positions) to none.
constexpr u8 stream_audio_channel_inputs(const char* channel) noexcept {
    constexpr const char* kLeft[] = {"FL", "SL", "RL", "FLC", "FLW"};
    constexpr const char* kRight[] = {"FR", "SR", "RR", "FRC", "FRW"};
    constexpr const char* kBoth[] = {"MONO", "FC", "RC", "TC"};
    for (const char* name : kLeft)
        if (stream_audio_text_equal(channel, name))
            return 1;
    for (const char* name : kRight)
        if (stream_audio_text_equal(channel, name))
            return 2;
    for (const char* name : kBoth)
        if (stream_audio_text_equal(channel, name))
            return 3;
    return 0;
}

} // namespace maicord
