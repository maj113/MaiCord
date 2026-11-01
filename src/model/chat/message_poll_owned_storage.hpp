#pragma once
// The poll store's single owned block: record storage, two text banks and the vote observation ring.
#include "maicord/core/arena.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/model/chat/message_poll_store.hpp"

namespace maicord {
inline constexpr u32 kVoteObservations = 128;
struct VoteObservation {
    u64 message_id;
    u64 user_id;
    u32 answer_id;
    bool voted;
};
struct MessagePollOwnedStorage {
    StableStorage records;
    Arena text_banks[2];
    VoteObservation observations[kVoteObservations];
    u32 next_observation;
    u8 active_bank;
};
static_assert(sizeof(MessagePollOwnedStorage) <= 4096);
} // namespace maicord
