#include "maicord/app/voice_transmit.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
// Mean square x256 at each level step: 32767^2 (full-scale square wave) times
// 10^((level - 100) / 10).
struct LevelTable {
    u64 energy[kVoiceLevelMaximum + 1];
};
constexpr LevelTable make_levels() noexcept {
    LevelTable table{};
    double value = 32767.0 * 32767.0 * 256.0;
    for (u32 level = kVoiceLevelMaximum + 1; level-- > 0;) {
        table.energy[level] = static_cast<u64>(value + 0.5);
        value /= 1.2589254117941673; // 10^0.1
    }
    return table;
}
constexpr LevelTable kLevels = make_levels();
constexpr u32 kFramerSamples = kVoiceFramerFrames * kVoiceFrameSamples;
constexpr u32 kFloorShift = 5, kFloorMargin = 18, kAutoMinimum = 20, kAutoMaximum = 80;
} // namespace

u32 voice_level_from_energy(u64 mean_square) noexcept {
    const u64 scaled = mean_square * 256;
    u32 low = 0, high = kVoiceLevelMaximum;
    if (scaled < kLevels.energy[0])
        return 0;
    while (low < high) {
        const u32 middle = (low + high + 1) / 2;
        if (kLevels.energy[middle] <= scaled)
            low = middle;
        else
            high = middle - 1;
    }
    return low;
}

u32 voice_level(ConstSlice<i16> pcm) noexcept {
    if (!pcm.data || !pcm.count)
        return 0;
    u64 sum = 0;
    for (u32 i = 0; i < pcm.count; ++i)
        sum += static_cast<u64>(static_cast<i32>(pcm[i]) * static_cast<i32>(pcm[i]));
    return voice_level_from_energy(sum / pcm.count);
}

void voice_apply_gain(Slice<i16> pcm, u32 gain_percent) noexcept {
    if (!pcm.data || gain_percent == 100)
        return;
    const i32 gain = static_cast<i32>(minimum(gain_percent, 200u));
    for (u32 i = 0; i < pcm.count; ++i) {
        const i32 scaled = pcm[i] * gain / 100;
        pcm[i] = static_cast<i16>(scaled > 32767 ? 32767 : scaled < -32768 ? -32768 : scaled);
    }
}

void voice_framer_push(VoiceFramer* framer, ConstSlice<i16> mono) noexcept {
    if (!framer || !mono.data)
        return;
    for (u32 at = 0; at < mono.count;) {
        if (framer->count == kFramerSamples) {
            // Stalled consumer: drop the oldest frame and keep time moving.
            framer->read = (framer->read + kVoiceFrameSamples) % kFramerSamples;
            framer->count -= kVoiceFrameSamples;
            ++framer->index;
            ++framer->dropped_frames;
        }
        const u32 write = (framer->read + framer->count) % kFramerSamples;
        const u32 take = minimum(minimum(mono.count - at, kFramerSamples - framer->count), kFramerSamples - write);
        copy_bytes(framer->samples + write, mono.data + at, take * static_cast<u32>(sizeof(i16)));
        framer->count += take;
        at += take;
    }
}

bool voice_framer_pop(VoiceFramer* framer, u64 now_us, Slice<i16> frame, u64* index, u64* wake_us) noexcept {
    if (wake_us)
        *wake_us = 0;
    if (!framer || !frame.data || frame.count < kVoiceFrameSamples || framer->count < kVoiceFrameSamples)
        return false;
    const u32 waiting = framer->count / kVoiceFrameSamples;
    // Re-anchor after a pause so a stale schedule cannot release a burst.
    if (!framer->next_due_us || now_us > framer->next_due_us + kVoiceFramerBacklog * kVoiceFrameUs)
        framer->next_due_us = now_us;
    if (waiting <= kVoiceFramerBacklog && now_us + kVoicePacingToleranceUs < framer->next_due_us) {
        if (wake_us)
            *wake_us = framer->next_due_us;
        return false;
    }
    for (u32 i = 0; i < kVoiceFrameSamples; ++i)
        frame[i] = framer->samples[(framer->read + i) % kFramerSamples];
    framer->read = (framer->read + kVoiceFrameSamples) % kFramerSamples;
    framer->count -= kVoiceFrameSamples;
    if (index)
        *index = framer->index;
    ++framer->index;
    framer->next_due_us += kVoiceFrameUs;
    return true;
}

bool voice_activity_update(VoiceActivity* activity, u32 level, u8 sensitivity, bool automatic) noexcept {
    if (!activity)
        return false;
    const u32 scaled = minimum(level, kVoiceLevelMaximum) << kFloorShift;
    if (!activity->floor_ready || scaled < activity->floor_scaled) {
        // Fast attack downward, but not to a single dropped-out frame.
        activity->floor_scaled = activity->floor_ready ? activity->floor_scaled - (activity->floor_scaled - scaled) / 2 : scaled;
        activity->floor_ready = true;
    } else
        ++activity->floor_scaled;
    u32 threshold = minimum<u32>(sensitivity, kVoiceLevelMaximum);
    if (automatic) {
        threshold = (activity->floor_scaled >> kFloorShift) + kFloorMargin;
        threshold = threshold < kAutoMinimum ? kAutoMinimum : threshold > kAutoMaximum ? kAutoMaximum : threshold;
    }
    activity->threshold = static_cast<u8>(threshold);
    if (level >= threshold) {
        activity->active = true;
        activity->hangover = kVoiceActivityHangoverFrames;
    } else if (activity->active) {
        if (activity->hangover)
            --activity->hangover;
        else
            activity->active = false;
    }
    return activity->active;
}

bool voice_push_to_talk_update(VoicePushToTalk* ptt, bool key_down, u64 now_us, u32 release_ms) noexcept {
    if (!ptt)
        return false;
    if (key_down) {
        ptt->active = true;
        ptt->release_us = 0;
    } else if (ptt->active) {
        if (ptt->down || !ptt->release_us)
            ptt->release_us = now_us + u64{release_ms} * 1000;
        if (now_us >= ptt->release_us) {
            ptt->active = false;
            ptt->release_us = 0;
        }
    }
    ptt->down = key_down;
    return ptt->active;
}

VoiceTransmitAction voice_transmit_next(VoiceTransmitter* transmitter, bool want) noexcept {
    if (!transmitter)
        return VoiceTransmitAction::idle;
    auto& t = *transmitter;
    if (want) {
        t.talking = true;
        t.tail = 0;
        if (t.announced)
            return VoiceTransmitAction::voice;
        t.announced = true;
        return VoiceTransmitAction::start;
    }
    if (t.talking) {
        t.talking = false;
        t.tail = kVoiceSilenceTailFrames;
    }
    if (!t.tail)
        return VoiceTransmitAction::idle;
    if (--t.tail)
        return VoiceTransmitAction::silence;
    t.announced = false;
    return VoiceTransmitAction::tail_end;
}

void voice_transmit_reset(VoiceTransmitter* transmitter) noexcept {
    if (transmitter)
        *transmitter = {};
}

u32 voice_rtp_header(Slice<u8> output, u16 sequence, u32 timestamp, u32 ssrc) noexcept {
    if (!output.data || output.count < kVoiceRtpHeaderBytes)
        return 0;
    output[0] = 0x80;
    output[1] = kVoiceOpusPayloadType;
    output[2] = static_cast<u8>(sequence >> 8);
    output[3] = static_cast<u8>(sequence);
    byte_store_be32(output.data + 4, timestamp);
    byte_store_be32(output.data + 8, ssrc);
    return kVoiceRtpHeaderBytes;
}
} // namespace maicord
