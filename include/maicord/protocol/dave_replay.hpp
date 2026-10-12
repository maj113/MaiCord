#pragma once
#include "maicord/core/base.hpp"

namespace maicord {
constexpr u32 kDaveMissingNonces = 1000;
constexpr u32 kDaveReplaySlots = kDaveMissingNonces;
constexpr u64 kDaveMaximumFrameCounter = (u64{1} << 56) - 1;
struct DaveFrameCounter {
    u64 value;
    u32 generation;
};
// The wire's high nonce byte is the low byte of a full uint32 key generation.
// Resolve it relative to the oldest retained generation, matching libdave.
// Key availability, expiry and forward-generation limits remain ratchet policy.
// Failure preserves output; neither generation nor the wide counter may wrap.
bool dave_frame_counter(u32 oldest_generation, u32 truncated_nonce, DaveFrameCounter* output) noexcept;

// One owner per sender/key epoch, shared by that sender's audio and video frames.
// Missing counters stay eligible until consumed or displaced by 1000 newer gaps,
// not merely until the largest nonce advances by 1000. The fixed ring keeps the
// 1000-entry bound without packet allocation or unused extra slots.
// Keep this with the active DAVE decryptor, not with every idle member/SSRC.
struct DaveReplay {
    u64 largest;
    u64 missing[kDaveReplaySlots];
    u32 head, count;
    bool initialized;
};
// Zero-initialize for a NEW sender epoch; retain across transport/source changes
// for the same sender epoch. Worker-only: check, authenticate, then commit.
// Authentication failure must not commit. These helpers do not authenticate,
// enforce key expiry or provide transport replay protection on their own.
bool dave_replay_can_accept(const DaveReplay& replay, u64 counter) noexcept;
bool dave_replay_commit(DaveReplay* replay, u64 authenticated_counter) noexcept;
} // namespace maicord
