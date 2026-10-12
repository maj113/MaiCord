#include "maicord/model/chat_state.hpp"
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
        "channel name: empty and identical names admit without allocation or metadata publication");
    const auto append = name_input(utf8_literal("abcdefghij"));
    check(s.name_used == 6 && name_admits(append, ChatStateStoreError::none),
        "channel name: exact append tail admits with every old name still live");
    check(chat_state_store_upsert_channel(&s, append).ok() && s.name_used == 16 && s.name_wasted == 3 &&
        s.compactions == 0 && s.active_name_bank == 0 && name_fixture.channels[0].name_offset == 6 &&
        utf8_equal(chat_state_store_channel_name(s, name_fixture.channels[0]), utf8_literal("abcdefghij")),
        "channel name: append admission matches actual allocation offset used wasted and bank counters");

    check(name_seed(16), "channel name: compacting fixture resets");
    s.name_used = 16; s.name_wasted = 10;
    const auto compact = name_input(utf8_literal("abcdefgh"));
    check(name_admits(compact, ChatStateStoreError::none),
        "channel name: tail exhaustion admits actual live-plus-new allocation before old name retirement");
    check(chat_state_store_upsert_channel(&s, compact).ok() && s.name_used == 14 && s.name_wasted == 3 &&
        s.compactions == 1 && s.active_name_bank == 1 && name_fixture.channels[0].name_offset == 22 &&
        utf8_equal(chat_state_store_guild_name(s, name_fixture.guilds[0]), utf8_literal("G")) &&
        utf8_equal(chat_state_store_channel_name(s, name_fixture.channels[1]), utf8_literal("Q")) &&
        utf8_equal(chat_state_store_role_name(s, name_fixture.roles[0]), utf8_literal("R")) &&
        name_admits(compact, ChatStateStoreError::none),
        "channel name: compaction preserves namespaces and the identical fast path validates an active second bank");
    check(name_seed(16, utf8_literal("12345678"), utf8_literal("1234567"), {}),
        "channel name: allocation-before-retirement boundary fixture");
    check(s.name_used == 16 && name_admits(name_input(utf8_literal("abcdef")), ChatStateStoreError::name_capacity),
        "channel name: final names fitting cannot admit transient compaction allocation with old name still live");

    check(name_seed(32), "channel name: private clear fixture resets");
    ChannelInput private_input{}; private_input.id = 90; private_input.type = 3;
    private_input.name = utf8_literal("Group"); private_input.name_known = true;
    check(chat_state_store_upsert_channel(&s, private_input).ok(), "channel name: named private channel publishes");
    const u32 private_used = s.name_used;
    private_input.name = {};
    check(name_admits(private_input, ChatStateStoreError::none) &&
        chat_state_store_upsert_channel(&s, private_input).ok() && s.name_used == private_used &&
        s.name_wasted == 5 && name_fixture.channels[2].name_bytes == 0 && name_fixture.channels[2].name_offset == 0,
        "channel name: known empty private name clears existing label without allocating or compacting");

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

    const auto header = s;
    s.channel_count = s.channel_capacity + 1;
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: invalid channel count rejects before order probes"); s = header;
    s.guild_count = s.guild_capacity + 1;
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: invalid guild count retains header guard"); s = header;
    s.role_count = s.role_backed_count + 1;
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: live roles cannot exceed writable prefix"); s = header;
    s.overwrite_count = s.overwrite_backed_count + 1;
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: live overwrites cannot exceed writable prefix"); s = header;
    s.role_backed_count = s.role_capacity + 1;
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: backed roles cannot exceed logical cap"); s = header;
    s.overwrite_backed_count = s.overwrite_capacity + 1;
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: backed overwrites cannot exceed logical cap"); s = header;
    s.active_name_bank = 2;
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: invalid active bank retains header guard"); s = header;
    s.name_used = s.name_bank_bytes + 1;
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: invalid used tail cannot underflow append capacity"); s = header;
    s.names = nullptr;
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: missing name backing retains initialized header guard"); s = header;
    s.roles = nullptr;
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: nonzero backed role prefix requires its installed base"); s = header;
    s.overwrites = nullptr;
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: nonzero backed overwrite prefix requires its installed base"); s = header;
    name_fixture.channel_order[0] = static_cast<u16>(s.channel_capacity);
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: guarded lookup rejects out-of-range matched order slot");
    name_fixture.channel_order[0] = 0; name_fixture.channel_order[1] = static_cast<u16>(s.channel_capacity);
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: guarded lookup rejects out-of-range intermediate probe");
    name_fixture.channel_order[1] = 1; name_fixture.channels[0].occupied = false;
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: matched row must remain occupied");
    name_fixture.channels[0].occupied = true; name_fixture.channels[0].name_offset = s.name_bank_bytes + 1;
    check(name_admits(name_input(), ChatStateStoreError::invalid_input) &&
        name_admits(name_input(utf8_literal("old")), ChatStateStoreError::invalid_input),
        "channel name: target old span validates before empty or identical admission");
    name_fixture.channels[0].name_offset = 1; name_fixture.channels[0].name_bytes = 7;
    check(name_admits(name_input(), ChatStateStoreError::invalid_input), "channel name: target name cannot extend beyond used prefix");

    check(name_seed(16), "channel name: free-slot fixture resets");
    auto fresh = name_input(); fresh.id = 99;
    name_fixture.channels[2].occupied = true;
    check(name_admits(fresh, ChatStateStoreError::channel_capacity), "channel name: missing free slot rejects before empty-name fast path");
    name_fixture.channels[2].occupied = false;
    check(chat_state_store_upsert_channel(&s, {73, 7, 0, {}, 2, 0, 0}).ok() &&
        name_admits(fresh, ChatStateStoreError::channel_capacity), "channel name: full logical channel capacity rejects a new identity");

    check(name_seed(16), "channel name: fresh identity append fixture resets");
    fresh.name = utf8_literal("fresh");
    check(name_admits(fresh, ChatStateStoreError::none) &&
        chat_state_store_upsert_channel(&s, fresh).ok() && s.channel_count == 3 && s.name_used == 11 &&
        s.name_wasted == 0 && s.compactions == 0 && name_fixture.channels[2].id == 99 &&
        name_fixture.channels[2].name_offset == 6,
        "channel name: a new identity with a free slot admits exact append allocation without a global name walk");

    check(name_seed(16), "channel name: scoped consistency fixture resets");
    // Deliberately violate the initialized-store contract without dereferencing
    // that unrelated name. Fast admission is not a global model validator.
    name_fixture.roles[0].name_offset = sizeof(name_fixture.names) + 1;
    check(name_admits(name_input(), ChatStateStoreError::none) &&
        name_admits(name_input(utf8_literal("old")), ChatStateStoreError::none) &&
        name_admits(name_input(utf8_literal("append")), ChatStateStoreError::none),
        "channel name: no-compaction paths do not walk unrelated label metadata under their initialized-store contract");
    s.name_used = s.name_bank_bytes; s.name_wasted = 10;
    check(name_admits(name_input(utf8_literal("append")), ChatStateStoreError::invalid_input),
        "channel name: compaction path retains complete live-name bounds validation before admission");
}
}
