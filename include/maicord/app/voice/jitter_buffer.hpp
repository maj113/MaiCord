#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// 16 x 20 ms reorder window. Larger single-frame Opus packets than one slot
// are refused (real voice packets are a few hundred bytes).
inline constexpr u32 kVoiceJitterSlots = 16;
inline constexpr u32 kVoiceJitterPacketBytes = 1276;
inline constexpr u32 kVoiceJitterMinimumTarget = 2, kVoiceJitterMaximumTarget = 4; // Frames: 40-80 ms.
// Consecutive underrun frames concealed before the stream is treated as
// silent (DTX/talk-spurt end) and re-buffers on its next packet.
inline constexpr u32 kVoiceJitterConcealFrames = 5;
enum class VoiceJitterPush : u8 {
    stored,
    late,      // Older than the playout point.
    duplicate,
    rejected,  // Empty or oversized.
    resync     // Too far ahead of the window: older contents were discarded.
};
enum class VoiceJitterPop : u8 {
    packet,
    lost,      // Conceal one frame; `next` is the following packet when buffered (FEC source).
    buffering  // Not playing yet (or again): output silence.
};
struct VoiceJitterFrame {
    VoiceJitterPop kind;
    u16 sequence;
    u32 timestamp;
    ConstSlice<u8> packet, next; // Valid until the next push/pop/reset.
};
struct VoiceJitterSlot {
    u32 timestamp;
    u16 sequence, bytes;
    bool used;
    u8 data[kVoiceJitterPacketBytes];
};
struct VoiceJitterStats {
    u64 late, duplicate, rejected, trimmed;
};
// Per-SSRC adaptive buffer ordered by RTP sequence. Timestamps feed the
// RFC 3550 interarrival jitter estimate that selects the 40-80 ms target.
// Not synchronized: the owner serializes push (network) and pop (playout).
struct VoiceJitterBuffer {
    VoiceJitterSlot slots[kVoiceJitterSlots];
    VoiceJitterStats stats;
    u64 last_arrival_us;
    u32 last_timestamp, jitter_us, frame_us, target, count, underrun;
    u16 next_sequence, last_sequence, last_played;
    bool started, playing, has_last, has_played;
};
void voice_jitter_reset(VoiceJitterBuffer* buffer) noexcept;
VoiceJitterPush voice_jitter_push(VoiceJitterBuffer* buffer, u16 sequence, u32 timestamp, ConstSlice<u8> packet,
                                  u64 arrival_us) noexcept;
// Called once per frame by playout when it needs the next packet.
VoiceJitterFrame voice_jitter_pop(VoiceJitterBuffer* buffer) noexcept;
} // namespace maicord
