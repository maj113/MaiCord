#include "maicord/app/voice/transmit.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/simd_haswell.hpp"

namespace maicord {
namespace {
// The least mean square at each level step: 32767^2 (full-scale square wave) times 10^((level - 100) / 10), rounded at
// 1/256 and then up to a whole mean square, so a comparison of the mean square itself picks the level.
struct LevelTable {
    u32 energy[kVoiceLevelMaximum + 1];
};
constexpr LevelTable make_levels() noexcept {
    LevelTable table{};
    double value = 32767.0 * 32767.0 * 256.0;
    for (u32 level = kVoiceLevelMaximum + 1; level-- > 0;) {
        table.energy[level] = static_cast<u32>((static_cast<u64>(value + 0.5) + 255) / 256);
        value /= 1.2589254117941673; // 10^0.1
    }
    return table;
}
constexpr LevelTable kLevels = make_levels();
constexpr u32 kFramerSamples = kVoiceFramerFrames * kVoiceFrameSamples;
constexpr u32 kFloorShift = 5, kFloorMargin = 18, kAutoMinimum = 20, kAutoMaximum = 80;
} // namespace

u32 voice_level_from_energy(u64 mean_square) noexcept {
    u32 low = 0, high = kVoiceLevelMaximum;
    if (mean_square < kLevels.energy[0])
        return 0;
    while (low < high) {
        const u32 middle = (low + high + 1) / 2;
        if (kLevels.energy[middle] <= mean_square)
            low = middle;
        else
            high = middle - 1;
    }
    return low;
}

MAICORD_HASWELL u32 voice_level(ConstSlice<i16> pcm) noexcept {
    if (!pcm.data || !pcm.count)
        return 0;
    // The sum of two squares fits an unsigned 32-bit lane. The even and the odd lanes add up in 64-bit lanes.
    const __m256i even = _mm256_srli_epi64(_mm256_set1_epi32(-1), 32);
    __m256i squares = _mm256_setzero_si256();
    u32 i = 0;
    for (; i + 16 <= pcm.count; i += 16) {
        const __m256i samples = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(pcm.data + i));
        const __m256i pairs = _mm256_madd_epi16(samples, samples);
        squares = _mm256_add_epi64(squares, _mm256_and_si256(pairs, even));
        squares = _mm256_add_epi64(squares, _mm256_srli_epi64(pairs, 32));
    }
    const __v4du lanes = static_cast<__v4du>(squares);
    u64 sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
#pragma clang loop vectorize(disable) unroll(disable)
    for (; i < pcm.count; ++i)
        sum += static_cast<u64>(static_cast<i32>(pcm[i]) * static_cast<i32>(pcm[i]));
    return voice_level_from_energy(sum / pcm.count);
}

MAICORD_HASWELL void voice_apply_gain(Slice<i16> pcm, u32 gain_percent) noexcept {
    if (!pcm.data || gain_percent == 100)
        return;
    const i32 gain = static_cast<i32>(minimum(gain_percent, 200u));
    i16* samples = pcm.data;
    const u32 count = pcm.count;
    // As voice_mixer_accumulate: magnitude * gain / 100 = whole * gain + rest * gain / 100, in 16-bit lanes.
    // The limit is 32767 for a positive sample and 32768 for a negative sample.
    const __m256i hundredth = _mm256_set1_epi16(20972), hundred = _mm256_set1_epi16(100);
    const __m256i factor = _mm256_set1_epi16(static_cast<i16>(gain)), top = _mm256_set1_epi16(32767);
    u32 i = 0;
    for (; i + 16 <= count; i += 16) {
        auto* const at = reinterpret_cast<__m256i_u*>(samples + i);
        const __m256i sample = _mm256_loadu_si256(at);
        const __m256i magnitude = _mm256_abs_epi16(sample);
        const __m256i whole = _mm256_srli_epi16(_mm256_mulhi_epu16(magnitude, hundredth), 5);
        const __m256i rest = _mm256_sub_epi16(magnitude, _mm256_mullo_epi16(whole, hundred));
        const __m256i part = _mm256_srli_epi16(_mm256_mulhi_epu16(_mm256_mullo_epi16(rest, factor), hundredth), 5);
        const __m256i scaled = _mm256_adds_epu16(_mm256_mullo_epi16(whole, factor), part);
        const __m256i limit = _mm256_sub_epi16(top, _mm256_srai_epi16(sample, 15));
        _mm256_storeu_si256(at, _mm256_sign_epi16(_mm256_min_epu16(scaled, limit), sample));
    }
#pragma clang loop vectorize(disable) unroll(disable)
    for (; i < count; ++i) {
        const i32 scaled = samples[i] * gain / 100;
        samples[i] = static_cast<i16>(scaled > 32767 ? 32767 : scaled < -32768 ? -32768 : scaled);
    }
}

void voice_framer_push(VoiceFramer* framer, ConstSlice<i16> mono) noexcept {
    if (!mono.data)
        return;
    for (u32 at = 0; at < mono.count;) {
        if (framer->count == kFramerSamples) {
            // Stalled consumer: drop the oldest frame and keep time moving.
            framer->read = (framer->read + kVoiceFrameSamples) % kFramerSamples;
            framer->count -= kVoiceFrameSamples;
            ++framer->index;
        }
        const u32 write = (framer->read + framer->count) % kFramerSamples;
        const u32 take = minimum(minimum(mono.count - at, kFramerSamples - framer->count), kFramerSamples - write);
        copy_bytes(framer->samples + write, mono.data + at, take * static_cast<u32>(sizeof(i16)));
        framer->count += take;
        at += take;
    }
}

bool voice_framer_pop(VoiceFramer* framer, u64 now_us, Slice<i16> frame, u64* index, u64* wake_us) noexcept {
    *wake_us = 0;
    if (framer->count < kVoiceFrameSamples)
        return false;
    const u32 waiting = framer->count / kVoiceFrameSamples;
    // Re-anchor after a pause so a stale schedule cannot release a burst.
    if (!framer->next_due_us || now_us > framer->next_due_us + kVoiceFramerBacklog * kVoiceFrameUs)
        framer->next_due_us = now_us;
    if (waiting <= kVoiceFramerBacklog && now_us + kVoicePacingToleranceUs < framer->next_due_us) {
        *wake_us = framer->next_due_us;
        return false;
    }
    // Frames sit on 960-sample boundaries of the ring (read only ever advances by one frame), so one never wraps.
    copy_bytes(frame.data, framer->samples + framer->read, kVoiceFrameSamples * static_cast<u32>(sizeof(i16)));
    framer->read = (framer->read + kVoiceFrameSamples) % kFramerSamples;
    framer->count -= kVoiceFrameSamples;
    *index = framer->index;
    ++framer->index;
    framer->next_due_us += kVoiceFrameUs;
    return true;
}

bool voice_activity_update(VoiceActivity* activity, u32 level, u8 sensitivity, bool automatic) noexcept {
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
    reset_value(transmitter);
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
