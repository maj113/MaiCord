#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/stable_storage.hpp"

namespace maicord {

// Discord keeps every guild's voice states. An owned store reserves this many
// and commits pages as it fills; at the bound whole guilds are evicted.
inline constexpr u32 kVoiceStateCapacity = 65'536;

// Bit values match GatewayVoiceFlag so Gateway records copy without translation.
enum VoiceFlag : u32 {
    voice_deaf = 1u << 0,
    voice_mute = 1u << 1,
    voice_self_deaf = 1u << 2,
    voice_self_mute = 1u << 3,
    voice_self_stream = 1u << 4,
    voice_self_video = 1u << 5,
    voice_suppress = 1u << 6,
    // Stage channels: request_to_speak_timestamp is set (see stage_policy.hpp).
    voice_request_to_speak = 1u << 7,
    // Client-local: the user is currently heard speaking. Never from Gateway;
    // preserved across Gateway updates and set by voice_state_store_set_speaking.
    voice_speaking = 1u << 31,
};

// Guild zero holds private-call states. Session IDs are never stored here.
struct VoiceState {
    u64 guild_id;
    u64 channel_id;
    u64 user_id;
    u32 flags;
    u32 request_to_speak_s; // Unix seconds of the raised hand (stage), else zero.
};

enum class VoiceStateResult : u8 {
    unchanged,
    changed,
    invalid_input,
    capacity,
};

// Records are sorted by channel and keep join order within one; a move to
// another channel re-joins at its end. Channel lookups are binary searches.
struct VoiceStateStore {
    // The writable prefix: owned stores grow it as they commit pages.
    Slice<VoiceState> records;
    u32 count;
    u32 revision;
    // States lost at the bound, and the guild evictions that freed room.
    u32 dropped;
    u32 evictions;
    // Never evicted (the selected guild); private calls never are either.
    u64 retained_guild;
    StableStorage owned;
};

// Requires zero live records and writable caller-owned storage. Leaves array
// bytes unchanged. Readers use count, and apply writes each complete new row.
bool voice_state_store_initialize_sparse(VoiceStateStore* store, Slice<VoiceState> records) noexcept;
// Reserves reserved_capacity states; nothing is committed until used.
bool voice_state_store_initialize_owned(VoiceStateStore* store, u32 reserved_capacity) noexcept;
void voice_state_store_release(VoiceStateStore* store) noexcept;
bool voice_state_store_reset(VoiceStateStore* store) noexcept;
// A zero channel removes the user's state in that guild.
VoiceStateResult voice_state_store_apply(VoiceStateStore* store, u64 guild_id,
                                                        u64 channel_id, u64 user_id, u32 flags,
                                                        u32 request_to_speak_s = 0) noexcept;
// Applies packed VoiceState-layout rows in order, as voice_state_store_apply
// would, sorting the joins once. Returns capacity when any row was dropped.
VoiceStateResult voice_state_store_apply_batch(VoiceStateStore* store, ConstSlice<u8> packed) noexcept;
VoiceStateResult voice_state_store_clear_guild(VoiceStateStore* store,
                                                              u64 guild_id) noexcept;
const VoiceState* voice_state_store_find(const VoiceStateStore& store, u64 guild_id,
                                                        u64 user_id) noexcept;
u32 voice_state_store_channel_count(const VoiceStateStore& store, u64 channel_id) noexcept;
// The index-th state in channel_id by join order, or null.
const VoiceState* voice_state_store_channel_at(const VoiceStateStore& store,
                                                              u64 channel_id, u32 index) noexcept;
// Go Live: true while the user streams (self_stream). LIVE badge hook.
inline bool voice_state_live(const VoiceState& state) noexcept {
    return (state.flags & voice_self_stream) != 0;
}
// Marks exactly the listed users of channel_id speaking and clears the
// mark everywhere else. True when anything changed.
bool voice_state_store_set_speaking(VoiceStateStore* store, u64 channel_id,
                                            ConstSlice<u64> users) noexcept;

} // namespace maicord
