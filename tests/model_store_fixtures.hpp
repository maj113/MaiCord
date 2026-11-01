#pragma once
// Caller-storage initializers for model stores. The shipping client only builds these stores
// itself (owned arenas, or the friends owner's own storage), so the fixed-capacity variants
// the tests rely on live here instead of in the model libraries.
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/relationship_store.hpp"
#include "maicord/model/voice_state_store.hpp"

namespace maicord {
// Initialize a zeroed or empty caller-owned store; storage stays stable.
inline bool relationship_store_initialize(RelationshipStore* store, Slice<RelationshipRecord> records) noexcept {
    if (!store || store->count || !records.data || !records.count || records.count > kRelationshipMaximumRecords ||
        !byte_range_valid({reinterpret_cast<const u8*>(records.data), records.count * static_cast<u32>(sizeof(*records.data))}) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(store), sizeof(*store)},
            {reinterpret_cast<const u8*>(records.data), records.count * static_cast<u32>(sizeof(*records.data))})) return false;
    const u32 revision = store->revision;
    *store = {records, 0, revision};
    for (u32 i = 0; i < records.count; ++i) reset_value(&records[i]);
    return true;
}

inline bool private_channel_store_initialize(PrivateChannelStore* store,
    Slice<PrivateChannelRecord> records, Slice<u64> recipients,
    Slice<PrivateNicknameRecord> nicknames = {}) noexcept {
    if (!store || !records.data || !records.count || !recipients.data || !recipients.count ||
        store->record_backing.base || store->recipient_backing.base || store->nickname_backing.base ||
        (!nicknames.data && nicknames.count)) return false;
    reset_value(store);
    store->records = records;
    store->recipients = recipients;
    store->nicknames = nicknames;
    return true;
}

inline bool voice_state_store_initialize_sparse(VoiceStateStore* store, Slice<VoiceState> records) noexcept {
    if (!store || store->count || store->owned.arena.base || !records.data || !records.count ||
        records.count > kVoiceStateCapacity)
        return false;
    reset_value(store);
    store->records = records;
    return true;
}
} // namespace maicord
