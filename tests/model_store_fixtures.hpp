#pragma once
// Caller-storage initializers for model stores. The shipping client only builds these stores
// itself (owned arenas, or the friends owner's own storage), so the fixed-capacity variants
// the tests rely on live here instead of in the model libraries.
#include "maicord/core/byte_range.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/user_activity.hpp"
#include "maicord/model/voice_channel_info_store.hpp"
#include "maicord/model/reaction_preferences.hpp"
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

// Upsert without a presence scope: an empty set clears the user's activities.
inline bool user_activity_upsert(UserActivityStore* store, const UserActivitySet& value) noexcept {
    return user_activity_apply(store, value, 0, false);
}

inline void voice_channel_info_store_reset(VoiceChannelInfoStore* store) noexcept {
    store->count = store->text_used = store->dropped = 0;
    ++store->revision;
}

inline bool reaction_preferences_is_favorite_emoji(const ReactionPreferenceStore& store, ReactionEmojiRef emoji) noexcept {
    const auto favorites = reaction_preferences_favorites(store);
    for (u32 index = 0; index < favorites.count; ++index)
        if (favorites[index] == emoji)
            return true;
    return false;
}

// Binds caller-owned role and overwrite rows. The shipping client only binds reserved address space
// (chat_state_store_bind_permissions_reserved), so the fixed-capacity variant lives here.
inline bool chat_state_store_bind_permissions(ChatStateStore* store, Slice<RoleRecord> roles,
                                              Slice<PermissionOverwriteRecord> overwrites) noexcept {
    if (!store || !store->guilds || !store->channels || !roles.data || roles.count == 0 ||
        roles.count > 65'535 || !overwrites.data || overwrites.count == 0 || overwrites.count > 65'535 ||
        store->roles || store->overwrites || store->role_count || store->overwrite_count ||
        store->permission_reserve || store->permission_reserve_context || store->role_backed_count ||
        store->overwrite_backed_count) {
        return false;
    }
    store->roles = roles.data;
    store->overwrites = overwrites.data;
    store->role_capacity = roles.count;
    store->overwrite_capacity = overwrites.count;
    store->role_backed_count = roles.count;
    store->overwrite_backed_count = overwrites.count;
    store->permission_reserve = nullptr;
    store->permission_reserve_context = nullptr;
    for (u32 index = 0; index < roles.count; ++index)
        reset_value<RoleRecord>(&roles[index]);
    for (u32 index = 0; index < overwrites.count; ++index)
        reset_value<PermissionOverwriteRecord>(&overwrites[index]);
    return true;
}
} // namespace maicord
