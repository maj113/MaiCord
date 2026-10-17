#include "maicord/app/quick_switcher_controller.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/permission_bits.hpp"

namespace maicord {
namespace {
struct SwitcherFixture {
    ChatStateStore state{};
    GuildRecord guilds[4]{};
    u16 guild_order[4]{};
    ChannelRecord channels[128]{};
    u16 channel_order[128]{};
    u8 names[32768]{};
    RoleRecord roles[4]{};
    PermissionOverwriteRecord overwrites[4]{};
    PrivateChannelStore private_channels{};
    PrivateChannelRecord private_records[8]{};
    u64 recipients[32]{};
    UserStore users{};
    UserRecord user_records[8]{};
    u32 user_order[8]{};
    u8 user_names[4096]{};
    RelationshipStore relationships{};
    RelationshipRecord relations[8]{};
    QuickSwitcherController controller{};
    QuickSwitcherScope scope{1, 2, 3};
    QuickSwitcherSources sources() const noexcept {
        return {&state, &users, &relationships};
    }
    bool initialize() noexcept {
        if (!chat_state_store_initialize(&state, {guilds, 4}, {guild_order, 4}, {channels, 128},
                                            {channel_order, 128}, {names, sizeof(names)}) ||
            !user_store_initialize(&users, {user_records, 8}, {user_order, 8},
                                           {user_names, sizeof(user_names)}) ||
            !private_channel_store_initialize(&private_channels, {private_records, 8}, {recipients, 32}) ||
            !relationship_store_initialize(&relationships, {relations, 8}) ||
            !chat_state_store_bind_permissions(&state, {roles, 4}, {overwrites, 4}))
            return false;
        state.private_channels = &private_channels;
        state.self_user_id = 3;
        return chat_state_store_upsert_guild(
                   &state, {10, utf8_literal("Workbench"), GuildMutationKind::create, 3})
                   .ok() &&
               chat_state_store_upsert_guild(
                   &state, {20, utf8_literal("Music"), GuildMutationKind::create, 3})
                   .ok() &&
               chat_state_store_upsert_role(
                   &state,
                   {10, 10, kPermissionViewChannel | kPermissionReadMessageHistory, {}}) &&
               chat_state_store_upsert_role(
                   &state,
                   {20, 20, kPermissionViewChannel | kPermissionReadMessageHistory, {}}) &&
               chat_state_store_set_self_roles(&state, 10, 3, 3, {}) &&
               chat_state_store_set_self_roles(&state, 20, 3, 3, {});
    }
    bool channel(u64 id, Utf8View name, u8 type = 0, u64 guild = 10, u64 parent = 0) noexcept {
        ChannelInput input{};
        input.id = id;
        input.guild_id = guild;
        input.name = name;
        input.type = type;
        input.parent_id = parent;
        if (!chat_state_store_upsert_channel(&state, input).ok())
            return false;
        if (guild)
            chat_state_store_recompute_guild_permissions(&state, guild);
        return true;
    }
    bool user(u64 id, Utf8View display, Utf8View username, RelationshipType type) noexcept {
        if (!user_store_upsert(&users, id, display, username).ok())
            return false;
        RelationshipRecord record{};
        record.user_id = id;
        record.type = type;
        return relationship_store_upsert(&relationships, record).ok();
    }
    bool sync(Utf8View query) noexcept {
        return quick_switcher_synchronize(&controller, scope, sources(), query);
    }
    const QuickSwitcherRow* find(u64 id) const noexcept {
        for (u32 i = 0; i < controller.count; ++i)
            if (controller.rows[i].target.id == id)
                return &controller.rows[i];
        return nullptr;
    }
};
} // namespace
void test_quick_switcher_controller(void (*check)(bool, const char*) noexcept) noexcept {
    SwitcherFixture f{};
    check(f.initialize() && f.channel(101, utf8_literal("general")) &&
              f.channel(102, utf8_literal("general-chat")) && f.channel(103, utf8_literal("engineering")) &&
              f.channel(104, utf8_literal("hidden")) && f.channel(105, utf8_literal("voice"), 2) &&
              f.channel(106, utf8_literal("forum"), 15) &&
              f.channel(107, utf8_literal("thread"), 11, 10, 101) &&
              f.channel(108, utf8_literal("other"), 0, 20),
          "quick switcher initializes authoritative stores and ordinary thread destinations");
    f.state.selected_channel_id = 103;
    f.state.selected_guild_id = 10;
    auto* hidden = const_cast<ChannelRecord*>(chat_state_store_find_channel(f.state, 104));
    hidden->permissions_resolved = false;
    hidden->actions = 0;
    check(f.sync(utf8_literal("#general")) && f.controller.count == 2 &&
              f.controller.rows[0].target.id == 101 && f.controller.rows[1].target.id == 102,
          "quick switcher ranks exact channel names before prefixes");
    const u32 revision = f.controller.revision, rebuilds = f.controller.rebuild_count;
    const auto scans = f.controller.stats.visited_candidates;
    check(!f.sync(utf8_literal("#general")) && f.controller.revision == revision &&
              f.controller.rebuild_count == rebuilds && f.controller.stats.visited_candidates == scans,
          "quick switcher retains unchanged query and source without rescanning or publishing rows");
    QuickSwitcherTarget target{};
    check(quick_switcher_resolve(f.controller, f.scope, f.sources(), 0, revision, &target) &&
              target.channel_id == 101,
          "quick switcher resolves a current bounded channel route");
    auto* first = const_cast<ChannelRecord*>(chat_state_store_find_channel(f.state, 101));
    first->actions = 0;
    ++f.state.permission_mutations;
    check(!quick_switcher_resolve(f.controller, f.scope, f.sources(), 0, revision, &target) && !target.id &&
              f.sync(utf8_literal("#general")) && f.controller.count == 1 &&
              f.controller.rows[0].target.id == 102,
          "permission retirement fences a captured route and removes inaccessible results");
    first->actions = channel_action_view;
    ++f.state.permission_mutations;
    check(f.sync(utf8_literal("#")) && !f.find(103) && !f.find(104) && !f.find(105) && !f.find(106) &&
              f.find(107),
          "channel search excludes current hidden voice and forum records while admitting live threads");
    check(f.sync(utf8_literal("*mus")) && f.controller.count == 1 &&
              f.controller.rows[0].target.guild_id == 20 &&
              f.controller.rows[0].target.kind == QuickSwitcherKind::guild,
          "server prefix produces an existing server selection route");
    auto* voice = const_cast<ChannelRecord*>(chat_state_store_find_channel(f.state, 105));
    voice->actions &= ~static_cast<u32>(channel_action_connect);
    ++f.state.permission_mutations;
    check(f.sync(utf8_literal("!voice")) && f.controller.phase == QuickSwitcherPhase::ready && !f.controller.count,
          "voice prefix lists no channel the user cannot connect to");
    voice->actions |= channel_action_view | channel_action_connect;
    ++f.state.permission_mutations;
    check(f.sync(utf8_literal("!voi")) && f.controller.count == 1 &&
              f.controller.rows[0].target.kind == QuickSwitcherKind::voice_channel &&
              quick_switcher_resolve(f.controller, f.scope, f.sources(), 0, f.controller.revision, &target) &&
              target.channel_id == 105 && target.guild_id == 10,
          "voice prefix lists joinable voice channels as join routes");
    check(f.sync(utf8_literal("voice")) && f.find(105) && f.sync(utf8_literal("#voice")) && !f.find(105),
          "plain searches include voice channels; the channel prefix keeps text destinations only");
    voice->actions &= ~static_cast<u32>(channel_action_connect);
    ++f.state.permission_mutations;
    check(f.sync(utf8_literal("#general work")) && f.find(101) && f.find(102),
          "channel and server terms match together");
    check(f.sync(utf8_literal("#108")) && f.controller.count == 1 && f.controller.rows[0].target.id == 108,
          "quick switcher accepts an exact accessible channel snowflake");
    check(
        f.user(4, utf8_literal("Alice"), utf8_literal("alice_handle"), RelationshipType::friend_) &&
            f.user(5, utf8_literal("Blocked"), utf8_literal("blocked"), RelationshipType::blocked) &&
            f.user(6, utf8_literal("\xc3\x89lodie"), utf8_literal("elodie"),
                   RelationshipType::friend_) &&
            f.channel(201, {}, 1, 0) && f.channel(202, {}, 3, 0),
        "quick switcher prepares friends and private channels");
    const u64 alice[] = {4};
    const u64 group[] = {3, 4, 6};
    check(
        private_channel_store_replace(&f.private_channels, {201, 0, {alice, 1}, {}, true, false, false}) &&
            private_channel_store_replace(&f.private_channels,
                                          {202, 0, {group, 3}, {}, true, false, false}),
        "quick switcher fixture retains authoritative private recipients");
    check(f.sync(utf8_literal("@alice_handle")) && f.controller.count == 1 &&
              f.controller.rows[0].target.user_id == 4 &&
              f.controller.rows[0].target.kind == QuickSwitcherKind::private_channel &&
              f.controller.rows[0].target.channel_id == 201,
          "DM query searches username aliases and deduplicates a friend with an existing conversation");
    check(f.sync(utf8_literal("@alice")) && f.controller.count == 2 &&
              f.controller.rows[0].target.kind == QuickSwitcherKind::private_channel &&
              f.controller.rows[0].target.channel_id == 201,
          "equal friend and DM names preserve direct existing-conversation navigation");
    auto friend_alias = *relationship_store_find(f.relationships, 4);
    copy_bytes(friend_alias.nickname, "Ally Smith", 10);
    friend_alias.nickname_bytes = 10;
    check(relationship_store_upsert(&f.relationships, friend_alias).ok() &&
              f.sync(utf8_literal("@Ally Smith")) && f.controller.count == 1 &&
              f.controller.rows[0].target.kind == QuickSwitcherKind::private_channel &&
              f.controller.rows[0].target.channel_id == 201 && f.controller.rows[0].label_bytes == 10 &&
              bytes_equal(f.controller.rows[0].label, "Ally Smith", 10),
          "friend-only nickname match retains its best label while using an already accessible DM route");
    check(f.sync(utf8_literal("@\xc3\xa9LODIE")) && f.find(6) && !f.find(5),
          "friend search uses full Unicode lowercase and omits blocked accounts");
    check(f.sync(utf8_literal("@Alice \xc3\x89lodie")) && f.find(202) &&
              f.find(202)->target.kind == QuickSwitcherKind::group_private,
          "unnamed group DM titles remain searchable by recipients");
    f.sync(utf8_literal("@alice"));
    const u32 user_revision = f.controller.revision;
    auto* relation =
        const_cast<RelationshipRecord*>(relationship_store_find(f.relationships, 4));
    relation->type = RelationshipType::blocked;
    ++f.relationships.revision;
    check(!quick_switcher_current(f.controller, f.scope, f.sources(), user_revision),
          "relationship changes retire captured private/user results");
    f.sync(utf8_literal("#general"));
    const u32 before_generation = f.controller.revision;
    ++first->generation;
    check(!quick_switcher_resolve(f.controller, f.scope, f.sources(), 0, before_generation, &target),
          "live generation checks reject reused channel slots even before source publication");
    ++f.state.channel_mutations;
    f.sync(utf8_literal("#general"));
    auto newer_scope = f.scope;
    ++newer_scope.account_serial;
    check(!quick_switcher_current(f.controller, newer_scope, f.sources(), f.controller.revision),
          "account epoch fences an old switcher selection");
    check(quick_switcher_visit(&f.controller, f.scope, f.sources(), 101) &&
              quick_switcher_visit(&f.controller, f.scope, f.sources(), 102) &&
              !quick_switcher_visit(&f.controller, f.scope, f.sources(), 102) && f.sync({}) &&
              f.controller.count == 2 && f.controller.rows[0].target.id == 102 &&
              f.controller.rows[1].target.id == 101 && f.controller.rows[0].recent,
          "empty quick switcher preserves deduplicated navigation recency");
    f.state.selected_channel_id = 102;
    ++f.state.selection_revision;
    check(f.sync({}) && f.controller.count == 1 && f.controller.rows[0].target.id == 101,
          "empty recent list omits the current destination");
    check(f.sync(utf8_literal("\xc0\xaf")) && f.controller.phase == QuickSwitcherPhase::invalid_query &&
              !f.controller.count,
          "malformed UTF-8 query clears previously selectable results");
    char long_query[129]{};
    for (u32 i = 0; i < sizeof(long_query); ++i)
        long_query[i] = 'a';
    check(f.sync({long_query, sizeof(long_query)}) &&
              f.controller.phase == QuickSwitcherPhase::invalid_query && !f.controller.count,
          "oversize quick query fails before copying into bounded storage");
    f.scope = newer_scope;
    f.sync({});
    check(!f.controller.recent_count,
          "account switch clears navigation history belonging to another account");

    // Real model admission feeds more than one hundred candidates; no result cursor silently grows storage.
    SwitcherFixture large{};
    check(large.initialize(), "large quick switcher fixture initializes");
    for (u32 i = 0; i < 110; ++i)
        check(large.channel(1000 + i, utf8_literal("general")), "large quick switcher channel admitted");
    large.state.selected_channel_id = 0;
    ++large.state.selection_revision;
    check(large.sync(utf8_literal("#general")) && large.controller.count == 100 &&
              large.controller.overflow && large.controller.stats.matched_candidates == 110 &&
              large.controller.rows[0].target.id == 1000 && large.controller.rows[99].target.id == 1099,
          "explicit channel query retains deterministic best hundred and reports result truncation");
    check(large.sync(utf8_literal("general")) && large.controller.count == 5 && large.controller.overflow,
          "general quick switcher matches remain bounded to five ranked destinations");
    for (u32 i = 0; i < 10; ++i)
        quick_switcher_visit(&large.controller, large.scope, large.sources(), 1000 + i);
    check(large.controller.recent_count == 8 && large.controller.recent_channels[0] == 1009 &&
              large.controller.recent_channels[7] == 1002 && large.sync({}) && large.controller.count == 7,
          "navigation history retains eight destinations and renders seven recent entries");
}
} // namespace maicord
