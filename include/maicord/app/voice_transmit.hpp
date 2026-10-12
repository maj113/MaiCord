#pragma once
#include "maicord/app/voice_audio_settings.hpp"
#include "maicord/core/slice.hpp"

namespace maicord {
// Send-side building blocks shared by the capture thread and tests: 20 ms
// framing with pacing, level metering, voice activity, push-to-talk, the
// speaking/silence-tail state machine and RTP headers. No I/O, no threads.
inline constexpr u32 kVoiceFrameSamples = 960; // 20 ms of 48 kHz mono.
inline constexpr u64 kVoiceFrameUs = 20'000;
inline constexpr u32 kVoiceRtpHeaderBytes = 12;
inline constexpr u8 kVoiceRtpOpusType = 120;
inline constexpr u8 kVoiceSilenceFrame[3]{0xf8, 0xff, 0xfe};
inline constexpr u32 kVoiceSilenceTailFrames = 5;
inline constexpr u32 kVoiceActivityHangoverFrames = 15; // 300 ms.

// Mean-square level of PCM16 on the 0..100 (-100..0 dBFS) scale.
u32 voice_level(ConstSlice<i16> pcm) noexcept;
u32 voice_level_from_energy(u64 mean_square) noexcept;
// Scales PCM16 in place by 0..200 % with saturation.
void voice_apply_gain(Slice<i16> pcm, u32 gain_percent) noexcept;

// Capture samples in, paced 20 ms frames out. Frames leave no earlier than
// 20 ms apart unless more than kVoiceFramerBacklog frames wait (device clock
// running ahead); a stall beyond the ring drops the oldest audio. index
// counts every captured frame, sent or dropped, so RTP timestamps follow
// the capture clock across gaps.
inline constexpr u32 kVoiceFramerFrames = 8;
inline constexpr u32 kVoiceFramerBacklog = 2;
inline constexpr u64 kVoicePacingToleranceUs = 2'000;
struct VoiceFramer {
    i16 samples[kVoiceFramerFrames * kVoiceFrameSamples];
    u32 read, count;
    u64 index, next_due_us, dropped_frames;
};
void voice_framer_push(VoiceFramer* framer, ConstSlice<i16> mono) noexcept;
// One frame when a whole frame is buffered and pacing allows it at now_us.
// Otherwise false; *wake_us is when pacing next allows one (0 = wait for audio).
bool voice_framer_pop(VoiceFramer* framer, u64 now_us, Slice<i16> frame, u64* index, u64* wake_us) noexcept;

// Energy-threshold voice activity with hangover. Automatic mode follows the
// noise floor (fast down, ~1.5 dB/s up) and keeps the threshold 18 dB above it.
struct VoiceActivity {
    u32 floor_scaled; // Noise floor level in 1/32 steps.
    u32 hangover;
    u8 threshold;
    bool active, floor_ready;
};
bool voice_activity_update(VoiceActivity* activity, u32 level, u8 sensitivity, bool automatic) noexcept;

// Key held -> transmit at once; release keeps transmitting for the delay.
struct VoicePushToTalk {
    u64 release_us;
    bool down, active;
};
bool voice_push_to_talk_update(VoicePushToTalk* ptt, bool key_down, u64 now_us, u32 release_ms) noexcept;

// Per-frame decision. start precedes the first voice frame of a spurt
// (Speaking on); after the wish ends five silence frames follow and the last
// is tail_end (Speaking off afterwards). Wanting voice again during the tail
// resumes without a new start.
enum class VoiceTransmitAction : u8 {
    idle,
    start,
    voice,
    silence,
    tail_end
};
struct VoiceTransmitter {
    u8 tail;
    bool talking, announced;
};
VoiceTransmitAction voice_transmit_next(VoiceTransmitter* transmitter, bool want) noexcept;
// Abandons a spurt without a tail (transport gone).
void voice_transmit_reset(VoiceTransmitter* transmitter) noexcept;

u32 voice_rtp_header(Slice<u8> output, u16 sequence, u32 timestamp, u32 ssrc) noexcept;
} // namespace maicord
