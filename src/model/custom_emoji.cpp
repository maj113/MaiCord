#include "maicord/model/chat_state.hpp"

#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

bool valid_emoji_name(Utf8View name) noexcept {
    if (!name.data || name.bytes == 0 || name.bytes > kCustomEmojiNameMaximumBytes ||
        !utf8_validate(name).valid) {
        return false;
    }
    for (u32 index = 0; index < name.bytes; ++index) {
        const u8 value = static_cast<u8>(name.data[index]);
        if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == '_')) {
            return false;
        }
    }
    return true;
}

bool read_role(ConstSlice<u8> roles, u32 index, u64* role_id) noexcept {
    const u64 offset = static_cast<u64>(index) * 8;
    if (!role_id || offset + 8 > roles.count) {
        return false;
    }
    const u64 value = byte_load_le64(roles.data + static_cast<u32>(offset));
    *role_id = value;
    return value != 0;
}

bool roles_valid(ConstSlice<u8> roles, u32 role_count) noexcept {
    if ((!roles.data && roles.count != 0) || role_count > kGuildRoleMaximum ||
        roles.count != role_count * 8u) {
        return false;
    }
    for (u32 index = 0; index < role_count; ++index) {
        u64 role_id = 0;
        if (!read_role(roles, index, &role_id)) {
            return false;
        }
        for (u32 previous = 0; previous < index; ++previous) {
            u64 previous_id = 0;
            if (!read_role(roles, previous, &previous_id) || previous_id == role_id) {
                return false;
            }
        }
    }
    return true;
}

u32 resident_emoji_count(const ChatStateStore& store, u64 guild_id) noexcept {
    u32 count = 0;
    for (u32 index = 0; index < store.emoji_count; ++index) {
        count += store.emojis[index].guild_id == guild_id ? 1u : 0u;
    }
    return count;
}

u32 resident_role_count(const ChatStateStore& store, u64 guild_id) noexcept {
    u32 count = 0;
    for (u32 index = 0; index < store.emoji_count; ++index) {
        if (store.emojis[index].guild_id == guild_id) {
            count += store.emojis[index].role_count;
        }
    }
    return count;
}

bool bind_emoji_storage(ChatStateStore* store, Slice<CustomEmojiRecord> emojis,
                         Slice<u64> emoji_roles, bool sparse) noexcept {
    if (!store || !store->guilds || store->emojis || store->emoji_roles || !emojis.data ||
        emojis.count == 0 || !emoji_roles.data || emoji_roles.count == 0 ||
        (sparse && (store->emoji_count || store->emoji_role_count))) {
        return false;
    }
    store->emojis = emojis.data;
    store->emoji_roles = emoji_roles.data;
    store->emoji_capacity = emojis.count;
    store->emoji_role_capacity = emoji_roles.count;
    if (!sparse) {
        for (u32 index = 0; index < emojis.count; ++index) {
            emojis[index] = CustomEmojiRecord{};
        }
        for (u32 index = 0; index < emoji_roles.count; ++index) {
            emoji_roles[index] = 0;
        }
    }
    return true;
}

} // namespace

bool chat_state_store_bind_emojis(ChatStateStore* store, Slice<CustomEmojiRecord> emojis,
                                     Slice<u64> emoji_roles) noexcept {
    return bind_emoji_storage(store, emojis, emoji_roles, false);
}

bool chat_state_store_bind_emojis_sparse(ChatStateStore* store, Slice<CustomEmojiRecord> emojis,
                                            Slice<u64> emoji_roles) noexcept {
    return bind_emoji_storage(store, emojis, emoji_roles, true);
}

const CustomEmojiRecord* chat_state_store_find_emoji(const ChatStateStore& store,
                                                         u64 emoji_id) noexcept {
    if (!store.emojis || emoji_id == 0) {
        return nullptr;
    }
    for (u32 index = 0; index < store.emoji_count; ++index) {
        if (store.emojis[index].id == emoji_id) {
            return &store.emojis[index];
        }
    }
    return nullptr;
}

Utf8View chat_state_store_emoji_name(const CustomEmojiRecord& record) noexcept {
    return record.id != 0 && record.guild_id != 0 && record.name_bytes != 0 &&
                   record.name_bytes <= kCustomEmojiNameMaximumBytes
               ? Utf8View{record.name, record.name_bytes}
               : Utf8View{};
}

bool chat_state_store_emoji_usable(const ChatStateStore& store,
                                      const CustomEmojiRecord& record) noexcept {
    if (!store.emoji_roles || record.id == 0 || record.guild_id == 0 ||
        (record.flags & custom_emoji_available) == 0 || record.role_offset > store.emoji_role_count ||
        record.role_count > store.emoji_role_count - record.role_offset) {
        return false;
    }
    if (record.role_count == 0) {
        return true;
    }
    for (u32 index = 0; index < record.role_count; ++index) {
        if (chat_state_store_self_has_role(store, record.guild_id,
                                              store.emoji_roles[record.role_offset + index])) {
            return true;
        }
    }
    return false;
}

bool chat_state_store_can_replace_guild_emojis(const ChatStateStore& store, u64 guild_id,
                                                  u32 emoji_count, u32 role_count) noexcept {
    if (!store.emojis || !store.emoji_roles || guild_id == 0 || emoji_count > kGuildEmojiMaximum) {
        return false;
    }
    const u32 old_emojis = resident_emoji_count(store, guild_id);
    const u32 old_roles = resident_role_count(store, guild_id);
    return emoji_count <= store.emoji_capacity - (store.emoji_count - old_emojis) &&
           role_count <= store.emoji_role_capacity - (store.emoji_role_count - old_roles);
}

void chat_state_store_remove_guild_emojis(ChatStateStore* store, u64 guild_id) noexcept {
    if (!store || !store->emojis || !store->emoji_roles || guild_id == 0) {
        return;
    }
    u32 output = 0;
    for (u32 index = 0; index < store->emoji_count; ++index) {
        const CustomEmojiRecord record = store->emojis[index];
        if (record.guild_id != guild_id) {
            store->emojis[output++] = record;
        }
    }
    for (u32 index = output; index < store->emoji_count; ++index) {
        store->emojis[index] = CustomEmojiRecord{};
    }
    store->emoji_count = output;

    u32 role_output = 0;
    for (u32 index = 0; index < store->emoji_count; ++index) {
        CustomEmojiRecord& record = store->emojis[index];
        if (record.role_offset > store->emoji_role_count ||
            record.role_count > store->emoji_role_count - record.role_offset) {
            ++store->rejected;
            store->emoji_count = 0;
            store->emoji_role_count = 0;
            return;
        }
        const u32 source = record.role_offset;
        for (u32 role = 0; role < record.role_count; ++role) {
            store->emoji_roles[role_output + role] = store->emoji_roles[source + role];
        }
        record.role_offset = role_output;
        role_output += record.role_count;
    }
    for (u32 index = role_output; index < store->emoji_role_count; ++index) {
        store->emoji_roles[index] = 0;
    }
    store->emoji_role_count = role_output;
    saturating_increment(store->emoji_mutations);
}

bool chat_state_store_replace_guild_emojis(ChatStateStore* store, u64 guild_id,
                                              ConstSlice<CustomEmojiInput> emojis) noexcept {
    if (!store || guild_id == 0 || (!emojis.data && emojis.count != 0) ||
        emojis.count > kGuildEmojiMaximum) {
        return false;
    }
    u32 role_count = 0;
    for (u32 index = 0; index < emojis.count; ++index) {
        const CustomEmojiInput& input = emojis[index];
        if (input.id == 0 || !valid_emoji_name(input.name) ||
            !roles_valid(input.role_ids_le, input.role_count) || role_count > ~0u - input.role_count) {
            ++store->rejected;
            return false;
        }
        role_count += input.role_count;
        for (u32 previous = 0; previous < index; ++previous) {
            if (emojis[previous].id == input.id) {
                ++store->rejected;
                return false;
            }
        }
    }
    if (!chat_state_store_can_replace_guild_emojis(*store, guild_id, emojis.count, role_count)) {
        ++store->rejected;
        return false;
    }
    chat_state_store_remove_guild_emojis(store, guild_id);
    for (u32 index = 0; index < emojis.count; ++index) {
        const CustomEmojiInput& input = emojis[index];
        CustomEmojiRecord& record = store->emojis[store->emoji_count++];
        record = CustomEmojiRecord{};
        record.id = input.id;
        record.guild_id = guild_id;
        record.role_offset = store->emoji_role_count;
        record.role_count = input.role_count;
        record.name_bytes = static_cast<u8>(input.name.bytes);
        record.flags = input.flags;
        copy_bytes(record.name, input.name.data, input.name.bytes);
        for (u32 role = 0; role < input.role_count; ++role) {
            u64 role_id = 0;
            if (!read_role(input.role_ids_le, role, &role_id)) {
                ++store->rejected;
                return false;
            }
            store->emoji_roles[store->emoji_role_count++] = role_id;
        }
    }
    return true;
}

} // namespace maicord
