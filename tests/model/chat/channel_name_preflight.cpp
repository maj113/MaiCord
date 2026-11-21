#include "maicord/model/chat/chat_state.hpp"
#include "../../support/model_store_fixtures.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
struct ChannelNameFixture {
    ChatStateStore state;
    GuildRecord guilds[2];
    ChannelRecord channels[3];
    RoleRecord roles[2];
    PermissionOverwriteRecord overwrites[2];
    u16 guild_order[2], channel_order[3];
    u8 names[256];
};
ChannelNameFixture name_fixture{}, name_before{};
bool name_seed(u32 bank, Utf8View target = utf8_literal("old"),
               Utf8View other = utf8_literal("Q"), Utf8View role = utf8_literal("R")) noexcept {
    name_fixture = {};
    auto& f = name_fixture;
    return bank <= sizeof(f.names) / 2 &&
        chat_state_store_initialize(&f.state, {f.guilds, 2}, {f.guild_order, 2},
            {f.channels, 3}, {f.channel_order, 3}, {f.names, bank * 2}) &&
        chat_state_store_bind_permissions(&f.state, {f.roles, 2}, {f.overwrites, 2}) &&
        chat_state_store_upsert_guild(&f.state, {7, utf8_literal("G"), GuildMutationKind::create, 99}).ok() &&
        chat_state_store_upsert_channel(&f.state, {71, 7, 0, target, 0, 0, 0}).ok() &&
        chat_state_store_upsert_channel(&f.state, {72, 7, 0, other, 1, 0, 0}).ok() &&
        chat_state_store_upsert_role(&f.state, {7, 7, 0, role});
}
ChannelInput name_input(Utf8View name = {}) noexcept {
    ChannelInput input{}; input.id = 71; input.guild_id = 7;
    input.name = name; input.name_known = true;
    return input;
}
bool name_admits(const ChannelInput& input, ChatStateStoreError expected) noexcept {
    // Snapshot the same original object's representation, including unused
    // backing and padding. This is an immutability assertion, not equality of
    // padding between independently constructed semantic objects.
    copy_bytes(&name_before, &name_fixture, sizeof(name_fixture));
    return chat_state_store_preflight_channel(name_fixture.state, input) == expected &&
        bytes_equal(&name_fixture, &name_before, sizeof(name_fixture));
}
}

void test_channel_name_preflight(void (*check)(bool, const char*) noexcept) noexcept {
    check(name_seed(16), "channel name: initialized named guild/channel/role fixture");
    auto& s = name_fixture.state;
    check(name_admits(name_input(), ChatStateStoreError::none) &&
        name_admits(name_input(utf8_literal("old")), ChatStateStoreError::none),
        "channel name: empty and identical names admit without changing the store");
    const auto append = name_input(utf8_literal("abcdefghij"));
    check(name_admits(append, ChatStateStoreError::none),
        "channel name: a name that fits the free tail admits");
    check(chat_state_store_upsert_channel(&s, append).ok() &&
        utf8_equal(chat_state_store_channel_name(s, name_fixture.channels[0]), utf8_literal("abcdefghij")),
        "channel name: the admitted append publishes the new name");

    check(name_seed(16, utf8_literal("0123456789")) && chat_state_store_upsert_channel(&s, name_input(utf8_literal("ab"))).ok(),
        "channel name: a shortened name leaves too little tail for a longer one");
    const auto compact = name_input(utf8_literal("abcdefgh"));
    check(name_admits(compact, ChatStateStoreError::none),
        "channel name: a name that fits only after compaction admits");
    check(chat_state_store_upsert_channel(&s, compact).ok() &&
        utf8_equal(chat_state_store_channel_name(s, *chat_state_store_find_channel(s, 71)), utf8_literal("abcdefgh")) &&
        utf8_equal(chat_state_store_guild_name(s, *chat_state_store_find_guild(s, 7)), utf8_literal("G")) &&
        utf8_equal(chat_state_store_channel_name(s, *chat_state_store_find_channel(s, 72)), utf8_literal("Q")) &&
        utf8_equal(chat_state_store_role_name(s, name_fixture.roles[0]), utf8_literal("R")) &&
        name_admits(compact, ChatStateStoreError::none),
        "channel name: compaction keeps guild, channel and role names");
    check(name_seed(16, utf8_literal("12345678"), utf8_literal("1234567"), {}),
        "channel name: allocation-before-retirement boundary fixture");
    check(name_admits(name_input(utf8_literal("abcdef")), ChatStateStoreError::name_capacity),
        "channel name: final names fitting cannot admit transient compaction allocation with old name still live");

    check(name_seed(32), "channel name: private clear fixture resets");
    ChannelInput private_input{}; private_input.id = 90; private_input.type = 3;
    private_input.name = utf8_literal("Group"); private_input.name_known = true;
    check(chat_state_store_upsert_channel(&s, private_input).ok(), "channel name: named private channel publishes");
    private_input.name = {};
    check(name_admits(private_input, ChatStateStoreError::none) &&
        chat_state_store_upsert_channel(&s, private_input).ok() &&
        !chat_state_store_channel_name(s, *chat_state_store_find_channel(s, 90)).bytes,
        "channel name: known empty private name clears existing label");

    check(name_seed(16), "channel name: invalid input fixture resets");
    auto invalid = name_input(); invalid.id = 0;
    check(name_admits(invalid, ChatStateStoreError::invalid_input), "channel name: empty name does not bypass zero identity");
    invalid = name_input(); invalid.guild_id = 8;
    check(name_admits(invalid, ChatStateStoreError::invalid_input), "channel name: empty name does not bypass resident guild identity");
    invalid = name_input(); invalid.type = 3; invalid.guild_id = 0; invalid.parent_id = 1;
    check(name_admits(invalid, ChatStateStoreError::invalid_input), "channel name: invalid private ownership fails before fast admission");
    invalid = name_input(); invalid.name = {nullptr, 1};
    check(name_admits(invalid, ChatStateStoreError::invalid_input), "channel name: null nonempty input is rejected");
    const char bad_utf8[]{static_cast<char>(0xC0), static_cast<char>(0xAF)};
    invalid.name = {bad_utf8, sizeof(bad_utf8)};
    check(name_admits(invalid, ChatStateStoreError::invalid_input), "channel name: UTF8 validation remains before append fit");
    char too_long[401]; for (char& value : too_long) value = 'x'; invalid.name = {too_long, sizeof(too_long)};
    check(name_admits(invalid, ChatStateStoreError::invalid_input), "channel name: 400-byte input limit remains before every fast path");

    check(name_seed(16), "channel name: free-slot fixture resets");
    auto fresh = name_input(); fresh.id = 99;
    check(chat_state_store_upsert_channel(&s, {73, 7, 0, {}, 2, 0, 0}).ok() &&
        name_admits(fresh, ChatStateStoreError::channel_capacity), "channel name: full logical channel capacity rejects a new identity");

    check(name_seed(16), "channel name: fresh identity append fixture resets");
    fresh.name = utf8_literal("fresh");
    check(name_admits(fresh, ChatStateStoreError::none) &&
        chat_state_store_upsert_channel(&s, fresh).ok() && s.channel_count == 3 &&
        utf8_equal(chat_state_store_channel_name(s, *chat_state_store_find_channel(s, 99)), utf8_literal("fresh")),
        "channel name: a new identity with a free slot admits and publishes its name");
}
}
