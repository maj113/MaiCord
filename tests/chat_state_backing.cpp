#include "maicord/model/chat_state.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u32 kSlots = 32;
constexpr u32 kBank = 256;
constexpr u8 kUncommitted = 0xFF;

struct BackingFixture {
    ChatStateStore state;
    GuildRecord guilds[2];
    u16 guild_order[2];
    ChannelRecord channels[kSlots];
    u16 channel_order[kSlots];
    u8 names[kBank * 2];
    CustomEmojiRecord emojis[8];
    u64 emoji_roles[8];
    u32 channel_limit;
    u32 name_limit[2];
    u32 emoji_limit;
    u32 channel_committed;
    u32 name_committed[2];
    u32 emoji_committed;
    u32 emoji_role_committed;
};
BackingFixture backing{};

// Commits zeroed storage up to the test's limits. Uncommitted storage holds
// 0xFF, so a store that reads before committing sees corrupt records.
bool commit(void* context, ChatStateBacking kind, u32 bank, u32 required) noexcept {
    auto& f = *static_cast<BackingFixture*>(context);
    if (kind == ChatStateBacking::emojis || kind == ChatStateBacking::emoji_roles) {
        if (required > f.emoji_limit)
            return false;
        if (kind == ChatStateBacking::emojis) {
            for (u32 i = f.emoji_committed; i < required; ++i)
                f.emojis[i] = CustomEmojiRecord{};
            f.emoji_committed = maximum(f.emoji_committed, required);
        } else {
            for (u32 i = f.emoji_role_committed; i < required; ++i)
                f.emoji_roles[i] = 0;
            f.emoji_role_committed = maximum(f.emoji_role_committed, required);
        }
        return true;
    }
    if (kind == ChatStateBacking::channels) {
        if (required > f.channel_limit)
            return false;
        for (u32 i = f.channel_committed; i < required; ++i) {
            f.channels[i] = ChannelRecord{};
            f.channel_order[i] = 0;
        }
        f.channel_committed = maximum(f.channel_committed, required);
        return true;
    }
    if (bank > 1 || required > f.name_limit[bank])
        return false;
    for (u32 i = f.name_committed[bank]; i < required; ++i)
        f.names[bank * kBank + i] = 0;
    f.name_committed[bank] = maximum(f.name_committed[bank], required);
    return true;
}

bool seed(u32 channel_limit, u32 name_limit) noexcept {
    backing = {};
    auto& f = backing;
    // The emoji records and role ids are adjacent: one pass poisons both.
    const auto poison = [](void* data, usize bytes) noexcept { memset(data, kUncommitted, bytes); };
    poison(f.channels, sizeof(f.channels));
    poison(f.channel_order, sizeof(f.channel_order));
    poison(f.names, sizeof(f.names));
    poison(f.emojis, sizeof(f.emojis) + sizeof(f.emoji_roles));
    f.channel_limit = channel_limit;
    f.name_limit[0] = f.name_limit[1] = name_limit;
    f.emoji_limit = 8;
    return chat_state_store_initialize(&f.state, {f.guilds, 2}, {f.guild_order, 2},
                                       {f.channels, kSlots}, {f.channel_order, kSlots},
                                       {f.names, kBank * 2}, true) &&
           chat_state_store_bind_backing(&f.state, commit, &f) &&
           chat_state_store_bind_emojis_sparse(&f.state, {f.emojis, 8}, {f.emoji_roles, 8}) &&
           chat_state_store_upsert_guild(&f.state, {7, utf8_literal("G"), GuildMutationKind::create, 99})
               .ok();
}

constexpr const char* kNames[] = {"c0", "c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9"};

Utf8View text(const char* value) noexcept {
    return {value, c_string_length(value)};
}

ChannelInput channel(u64 id, Utf8View name) noexcept {
    ChannelInput input{};
    input.id = id;
    input.guild_id = 7;
    input.name = name;
    input.name_known = true;
    return input;
}

bool named(u64 id, Utf8View expected) noexcept {
    const auto* record = chat_state_store_find_channel(backing.state, id);
    return record && utf8_equal(chat_state_store_channel_name(backing.state, *record), expected);
}

constexpr u8 kRoleLe[8] = {5, 0, 0, 0, 0, 0, 0, 0};
CustomEmojiInput emoji(u64 id, const char* name) noexcept {
    return CustomEmojiInput{id, text(name), {kRoleLe, 8}, 1, custom_emoji_available, 0};
}

struct Batch {
    u64 first_id;
    u32 count;
};
bool read_batch(const void* context, u32 index, ChannelInput* output) noexcept {
    const auto& batch = *static_cast<const Batch*>(context);
    if (index >= batch.count)
        return false;
    *output = channel(batch.first_id + index, utf8_literal("batch"));
    return true;
}
} // namespace

void test_chat_state_backing(void (*check)(bool, const char*) noexcept) noexcept {
    // Channels and names are committed just ahead of use, one slot and one
    // name at a time; nothing past the slot bound is ever read.
    bool grew = seed(kSlots, kBank) && chat_state_store_channel_slot_bound(backing.state) == 0;
    for (u32 i = 0; grew && i < 10; ++i)
        grew = chat_state_store_upsert_channel(&backing.state, channel(100 + i, text(kNames[i]))).ok();
    check(grew && backing.state.channel_count == 10 && backing.channel_committed == 10 &&
              chat_state_store_channel_slot_bound(backing.state) == 10 &&
              backing.name_committed[0] == backing.state.name_used && backing.name_committed[1] == 0 &&
              named(100, utf8_literal("c0")) && named(109, utf8_literal("c9")),
          "chat state backing commits each channel slot and name byte just before writing it");

    // A freed slot is reused without committing more.
    check(chat_state_store_remove_channel(&backing.state, 103) &&
              chat_state_store_upsert_channel(&backing.state, channel(200, utf8_literal("re"))).ok() &&
              backing.channel_committed == 10 && backing.state.channel_count == 10 &&
              named(200, utf8_literal("re")),
          "chat state backing reuses a freed slot inside the committed range");

    // When the next slot cannot be committed, the insert is refused and the
    // model is unchanged.
    bool refused = seed(3, kBank);
    for (u32 i = 0; refused && i < 3; ++i)
        refused = chat_state_store_upsert_channel(&backing.state, channel(100 + i, text(kNames[i]))).ok();
    const u32 used = backing.state.name_used;
    const auto rejected = chat_state_store_upsert_channel(&backing.state, channel(150, utf8_literal("no")));
    check(refused && rejected.error == ChatStateStoreError::channel_capacity &&
              backing.state.channel_count == 3 &&
              backing.state.name_used == used && !chat_state_store_find_channel(backing.state, 150) &&
              chat_state_store_channel_slot_bound(backing.state) == 3,
          "chat state backing refuses a channel whose slot cannot be committed, without mutation");

    // Batch admission commits the whole batch's slots and names up front, or
    // refuses the batch before anything changes.
    const Batch fits{300, 4}, too_many{300, 6};
    const auto admit = [](const Batch& batch) noexcept {
        return chat_state_store_preflight_channel_batch(backing.state, batch.count, read_batch, &batch);
    };
    check(seed(5, kBank) &&
              admit(too_many) == ChatStateStoreError::channel_capacity &&
              backing.state.channel_count == 0 &&
              admit(fits) == ChatStateStoreError::none &&
              backing.channel_committed == 4 && backing.name_committed[0] == 1 + 4 * 5,
          "chat state backing commits an admitted batch's slots and names before it applies");
    check(seed(kSlots, 1 + 2 * 5) &&
              admit(fits) == ChatStateStoreError::name_capacity &&
              backing.state.channel_count == 0 && backing.state.name_used == 1,
          "chat state backing refuses a batch whose names cannot be committed");

    // Compaction commits the other bank for every live name before moving
    // any; if it cannot, the names stay where they were.
    bool compacted = seed(kSlots, kBank);
    for (u32 i = 0; compacted && i < 200; ++i)
        compacted =
            chat_state_store_upsert_channel(&backing.state, channel(100, text(kNames[i % 10]))).ok();
    check(compacted && backing.state.compactions != 0 && backing.name_committed[1] != 0 &&
              named(100, text(kNames[9])),
          "chat state backing commits the target bank before compacting names into it");
    bool held = seed(kSlots, kBank);
    backing.name_limit[1] = 0;
    u32 renames = 0;
    for (; held && renames < 200; ++renames) {
        const auto renamed =
            chat_state_store_upsert_channel(&backing.state, channel(100, text(kNames[renames % 10])));
        if (!renamed.ok()) {
            held = renamed.error == ChatStateStoreError::name_capacity;
            break;
        }
    }
    check(held && renames < 200 && backing.state.compactions == 0 && backing.state.active_name_bank == 0 &&
              named(100, text(kNames[(renames + 9) % 10])),
          "chat state backing keeps names in place when the compaction target cannot be committed");

    // Emojis and their role IDs commit their final counts before a server's
    // set is replaced; a set that cannot be committed is refused unchanged.
    const CustomEmojiInput three[] = {emoji(901, "one"), emoji(902, "two"), emoji(903, "three")};
    check(seed(kSlots, kBank) && backing.emoji_committed == 0 &&
              chat_state_store_replace_guild_emojis(&backing.state, 7, {three, 3}) &&
              backing.emoji_committed == 3 && backing.emoji_role_committed == 3 &&
              chat_state_store_find_emoji(backing.state, 903) &&
              chat_state_store_replace_guild_emojis(&backing.state, 7, {three, 2}) &&
              backing.emoji_committed == 3 && backing.state.emoji_count == 2,
          "chat state backing commits a server's emojis and role IDs before replacing its set");
    check(seed(kSlots, kBank) && (backing.emoji_limit = 2, true) &&
              chat_state_store_guild_emoji_admission(backing.state, 7, 3, 3) ==
                  ChatStateStoreError::emoji_capacity &&
              !chat_state_store_replace_guild_emojis(&backing.state, 7, {three, 3}) &&
              backing.state.emoji_count == 0 && !chat_state_store_find_emoji(backing.state, 901),
          "chat state backing refuses an emoji set that cannot be committed, without mutation");
}
} // namespace maicord
