#include "maicord/app/voice_jitter_buffer.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u32 kDefaultFrameUs = 20'000;
constexpr i64 kDiscontinuityUs = 250'000;
// A packet this far behind the playout point is a restarted sequence, not a late arrival.
constexpr i32 kResyncBehind = -static_cast<i32>(kVoiceJitterSlots * 4);
i32 distance(u16 from, u16 to) noexcept {
    return static_cast<i16>(static_cast<u16>(to - from));
}
VoiceJitterSlot& slot(VoiceJitterBuffer& buffer, u16 sequence) noexcept {
    return buffer.slots[sequence % kVoiceJitterSlots];
}
bool holds(const VoiceJitterBuffer& buffer, u16 sequence) noexcept {
    const auto& value = buffer.slots[sequence % kVoiceJitterSlots];
    return value.used && value.sequence == sequence;
}
void clear(VoiceJitterBuffer& buffer) noexcept {
    for (auto& value : buffer.slots)
        value.used = false;
    buffer.count = 0;
    buffer.playing = false;
    buffer.underrun = 0;
}
void retarget(VoiceJitterBuffer& buffer) noexcept {
    // Cover roughly three standard deviations of arrival jitter plus one frame.
    const u32 frame = buffer.frame_us ? buffer.frame_us : kDefaultFrameUs;
    const u64 wanted = u64{kDefaultFrameUs} + u64{buffer.jitter_us} * 3;
    const u32 frames = static_cast<u32>((wanted + frame - 1) / frame);
    buffer.target = maximum(kVoiceJitterMinimumTarget, minimum(kVoiceJitterMaximumTarget, frames));
}
void observe(VoiceJitterBuffer& buffer, u16 sequence, u32 timestamp, u64 arrival_us) noexcept {
    if (buffer.has_last && sequence == static_cast<u16>(buffer.last_sequence + 1)) {
        const u32 samples = timestamp - buffer.last_timestamp;
        if (samples >= 120 && samples <= 5760)
            buffer.frame_us = samples * 125 / 6; // 48 kHz samples to microseconds.
        // RFC 3550 6.4.1: J += (|D| - J) / 16, using only consecutive packets.
        // A transit change beyond kDiscontinuityUs is a silence gap or sender
        // clock step (DTX without a timestamp jump), not network jitter.
        const i64 transit = static_cast<i64>(arrival_us - buffer.last_arrival_us) -
                            static_cast<i64>(u64{samples} * 125 / 6);
        const i64 magnitude = transit < 0 ? -transit : transit;
        if (magnitude <= kDiscontinuityUs) {
            buffer.jitter_us = static_cast<u32>(static_cast<i64>(buffer.jitter_us) +
                                                (magnitude - static_cast<i64>(buffer.jitter_us)) / 16);
            retarget(buffer);
        }
    }
    if (!buffer.has_last || distance(buffer.last_sequence, sequence) > 0) {
        buffer.last_sequence = sequence;
        buffer.last_timestamp = timestamp;
        buffer.last_arrival_us = arrival_us;
        buffer.has_last = true;
    }
}
// Oldest buffered sequence at or after the playout point.
bool oldest(const VoiceJitterBuffer& buffer, u16* sequence) noexcept {
    for (u32 i = 0; i < kVoiceJitterSlots; ++i)
        if (holds(buffer, static_cast<u16>(buffer.next_sequence + i))) {
            *sequence = static_cast<u16>(buffer.next_sequence + i);
            return true;
        }
    return false;
}
VoiceJitterFrame take(VoiceJitterBuffer& buffer) noexcept {
    auto& value = slot(buffer, buffer.next_sequence);
    value.used = false;
    --buffer.count;
    buffer.last_played = buffer.next_sequence++;
    buffer.has_played = true;
    buffer.underrun = 0;
    return {VoiceJitterPop::packet, value.sequence, value.timestamp, {value.data, value.bytes}, {}};
}
} // namespace

void voice_jitter_reset(VoiceJitterBuffer* buffer) noexcept {
    if (!buffer)
        return;
    clear(*buffer);
    buffer->stats = {};
    buffer->started = buffer->has_last = buffer->has_played = false;
    buffer->jitter_us = 0;
    buffer->frame_us = kDefaultFrameUs;
    buffer->target = kVoiceJitterMinimumTarget;
}

VoiceJitterPush voice_jitter_push(VoiceJitterBuffer* buffer, u16 sequence, u32 timestamp, ConstSlice<u8> packet,
                                  u64 arrival_us) noexcept {
    if (!buffer)
        return VoiceJitterPush::rejected;
    if (!packet.data || !packet.count || packet.count > kVoiceJitterPacketBytes) {
        ++buffer->stats.rejected;
        return VoiceJitterPush::rejected;
    }
    if (!buffer->started) {
        buffer->started = true;
        buffer->next_sequence = sequence;
        if (!buffer->target)
            retarget(*buffer);
    }
    auto result = VoiceJitterPush::stored;
    i32 ahead = distance(buffer->next_sequence, sequence);
    if (ahead < 0 && ahead >= kResyncBehind) {
        // While buffering, a reordered packet older than the first arrival
        // still plays unless it is at or before what already played.
        const bool unplayed = !buffer->has_played || distance(buffer->last_played, sequence) > 0;
        const bool fits = !buffer->count || distance(sequence, buffer->last_sequence) <
                                                static_cast<i32>(kVoiceJitterSlots);
        if (buffer->playing || !unplayed || !fits) {
            ++buffer->stats.late;
            return VoiceJitterPush::late;
        }
        buffer->next_sequence = sequence;
        ahead = 0;
    }
    if (ahead < 0 || ahead >= static_cast<i32>(kVoiceJitterSlots)) {
        clear(*buffer);
        buffer->next_sequence = sequence;
        buffer->has_last = buffer->has_played = false;
        ++buffer->stats.resync;
        result = VoiceJitterPush::resync;
    }
    auto& value = slot(*buffer, sequence);
    if (value.used) {
        ++buffer->stats.duplicate;
        return VoiceJitterPush::duplicate;
    }
    value.used = true;
    value.sequence = sequence;
    value.timestamp = timestamp;
    value.bytes = static_cast<u16>(packet.count);
    copy_bytes(value.data, packet.data, packet.count);
    ++buffer->count;
    ++buffer->stats.stored;
    observe(*buffer, sequence, timestamp, arrival_us);
    return result;
}

VoiceJitterFrame voice_jitter_pop(VoiceJitterBuffer* buffer) noexcept {
    VoiceJitterFrame frame{VoiceJitterPop::buffering};
    if (!buffer)
        return frame;
    if (!buffer->playing) {
        u16 first = 0;
        if (buffer->count < buffer->target || !oldest(*buffer, &first))
            return frame;
        buffer->next_sequence = first;
        buffer->playing = true;
        buffer->underrun = 0;
    }
    // Shed latency beyond the target, preferring the 3-byte silence frames.
    while (buffer->count > buffer->target + 2 && holds(*buffer, buffer->next_sequence) &&
           (slot(*buffer, buffer->next_sequence).bytes <= 3 || buffer->count > buffer->target + 4)) {
        take(*buffer);
        ++buffer->stats.trimmed;
    }
    if (holds(*buffer, buffer->next_sequence)) {
        ++buffer->stats.played;
        return take(*buffer);
    }
    frame.sequence = buffer->next_sequence;
    if (buffer->count) {
        // A later packet exists, so this one is lost rather than late.
        frame.kind = VoiceJitterPop::lost;
        const u16 following = static_cast<u16>(buffer->next_sequence + 1);
        if (holds(*buffer, following)) {
            const auto& value = slot(*buffer, following);
            frame.next = {value.data, value.bytes};
        }
        buffer->last_played = buffer->next_sequence++;
        buffer->has_played = true;
        buffer->underrun = 0;
        ++buffer->stats.lost;
        return frame;
    }
    // Empty: conceal in place (the packet may still arrive), then go quiet.
    ++buffer->stats.underruns;
    if (++buffer->underrun > kVoiceJitterConcealFrames) {
        buffer->playing = false;
        return frame;
    }
    frame.kind = VoiceJitterPop::lost;
    return frame;
}
} // namespace maicord
