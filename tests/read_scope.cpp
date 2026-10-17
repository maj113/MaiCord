#include "maicord/app/read_scope.hpp"
#include "maicord/model/permission_bits.hpp"
#include <limits>

namespace maicord {
namespace {
constexpr u64 permissions = kPermissionViewChannel | kPermissionReadMessageHistory;
constexpr u64 epoch = 1420070400000ull;
ReadScopeTarget full_targets[kReadScopeTargetMaximum];
ChannelRecord full_channels[kReadScopeTargetMaximum + 1];
u16 full_order[kReadScopeTargetMaximum + 1];
const ReadScopeTarget* find(ConstSlice<ReadScopeTarget> entries, u64 id) noexcept {
    for (u32 i = 0; i < entries.count; ++i)
        if (entries[i].channel_id == id)
            return &entries[i];
    return nullptr;
}
} // namespace
void test_read_scope(void (*check)(bool, const char*) noexcept) noexcept {
    ChatStateStore state{};
    GuildRecord guild{};
    ChannelRecord channels[24]{};
    u16 guild_order[1]{}, channel_order[24]{};
    u8 names[512]{};
    RoleRecord role{};
    PermissionOverwriteRecord overwrite{};
    check(
        chat_state_store_initialize(&state, {&guild, 1}, {guild_order, 1}, {channels, 24},
                                       {channel_order, 24}, {names, sizeof(names)}) &&
            chat_state_store_bind_permissions(&state, {&role, 1}, {&overwrite, 1}) &&
            chat_state_store_upsert_guild(&state, {1, {}, GuildMutationKind::create, 99}).ok() &&
            chat_state_store_upsert_role(&state, {1, 1, permissions, {}}) &&
            chat_state_store_set_self_roles(&state, 1, 99, 42, {}),
        "read scope fixture uses authoritative Core roles and current self");
    struct Input {
        u64 id, parent, latest;
        u8 type;
    };
    const Input inputs[]{{100, 0, 0, 4},       {101, 0, 0, 4},       {200, 100, 2000, 0},
                         {201, 100, 2010, 5},  {202, 100, 2020, 2},  {203, 100, 0, 13},
                         {204, 100, 2040, 15}, {205, 100, 0, 16},    {206, 101, 2060, 0},
                         {207, 100, 2070, 0},  {300, 200, 3000, 11}, {301, 204, 3010, 12},
                         {302, 200, 3020, 11}, {303, 200, 3030, 11}, {304, 200, 3040, 11},
                         {305, 200, 3050, 11}, {306, 999, 3060, 11}, {307, 100, 3070, 6}};
    bool inserted = true;
    for (u32 index = static_cast<u32>(sizeof(inputs) / sizeof(inputs[0])); index-- > 0;) {
        const auto& input = inputs[index];
        ChannelInput channel{input.id, 1, input.parent, {}, 0, 0, input.type};
        channel.last_message_id = input.latest;
        channel.last_message_known = true;
        if (channel_is_thread(input.type)) {
            channel.thread_self_joined_known = true;
            channel.thread_self_joined = true;
            channel.thread_active_known = true;
            channel.thread_listed_active = true;
            channel.thread_archived_known = true;
        }
        inserted = chat_state_store_upsert_channel(&state, channel).ok() && inserted;
    }
    chat_state_store_recompute_guild_permissions(&state, 1);
    auto mutable_channel = [&](u64 id) noexcept {
        return const_cast<ChannelRecord*>(chat_state_store_find_channel(state, id));
    };
    mutable_channel(207)->effective_permissions &= ~kPermissionReadMessageHistory;
    mutable_channel(206)->flags |= 128; // Resource flags are not a universal bulk exclusion.
    mutable_channel(302)->thread_archived = true;
    mutable_channel(303)->thread_listed_active = false;
    mutable_channel(304)->thread_self_joined = false;
    mutable_channel(305)->thread_active_known = false;
    mutable_channel(100)->collapsed = true;
    mutable_channel(200)->collapsed = true;
    state.selected_channel_id = 207;
    check(inserted, "read scope seeds text vocal forum media and positive current joined thread authority");
    ReadScopeTarget entries[24]{};
    auto plan = read_scope_plan(state, ReadScopeKind::guild, 1, epoch + 5, {entries, 24});
    const auto* forum = find({entries, plan.count}, 204);
    const auto* media = find({entries, plan.count}, 205);
    const auto* stage = find({entries, plan.count}, 203);
    check(plan.valid && plan.count == 8 && plan.outbound_count == 8 && forum && media && !stage &&
              forum->message_id == (5ull << 22) && media->message_id == forum->message_id &&
              find({entries, plan.count}, 206) && find({entries, plan.count}, 300) &&
              find({entries, plan.count}, 301),
          "server plan includes vocal resource and joined threads with one action clock for forum/media");
    const auto before_query = entries[0];
    const auto size = read_scope_plan_size(state, ReadScopeKind::guild, 1, epoch + 5);
    check(size.valid && size.count == plan.count && size.outbound_count == plan.outbound_count &&
              entries[0].channel_id == before_query.channel_id,
          "count-only plan admits the exact frozen prefix and wire count without touching output");
    bool sorted = true;
    for (u32 i = 1; i < plan.count; ++i)
        sorted = sorted && entries[i - 1].channel_id < entries[i].channel_id;
    check(sorted && !find({entries, plan.count}, 207) && !find({entries, plan.count}, 302) &&
              !find({entries, plan.count}, 303) && !find({entries, plan.count}, 304) &&
              !find({entries, plan.count}, 305) && !find({entries, plan.count}, 306) &&
              !find({entries, plan.count}, 307),
          "scope plan canonicalizes IDs and omits no-history archived inactive unknown unjoined orphan and "
          "unsupported records");
    mutable_channel(201)->last_read_message_id = 2010;
    const auto already_read = read_scope_plan(state, ReadScopeKind::guild, 1, epoch + 5, {entries, 24});
    check(already_read.valid && already_read.count == 7 && !find({entries, already_read.count}, 201) &&
              !find({entries, already_read.count}, 203),
          "server capture omits already-read and unknown-cursor ordinary channels before local clearing");
    mutable_channel(201)->mention_count = 1;
    const auto mentioned = read_scope_plan(state, ReadScopeKind::guild, 1, epoch + 5, {entries, 24});
    check(mentioned.valid && mentioned.count == 8 && find({entries, mentioned.count}, 201),
          "raw mentions retain a known-cursor server target despite cursor already read");
    mutable_channel(201)->mention_count = 0;
    mutable_channel(301)->thread_self_joined = false;
    const auto without_private = read_scope_plan(state, ReadScopeKind::guild, 1, epoch + 5, {entries, 24});
    check(without_private.valid && !find({entries, without_private.count}, 301),
          "private thread without current-self membership cannot enter server bulk scope");
    mutable_channel(301)->thread_self_joined = true;
    plan = read_scope_plan(state, ReadScopeKind::category, 100, epoch + 99, {entries, 24});
    forum = find({entries, plan.count}, 204);
    media = find({entries, plan.count}, 205);
    check(plan.valid && plan.count == 8 && plan.outbound_count == 6 && forum && forum->message_id == 2040 &&
              media && !media->outbound && !find({entries, plan.count}, 206) &&
              find({entries, plan.count}, 300) && find({entries, plan.count}, 301),
          "category uses child known cursors plus active joined descendants without a synthetic forum "
          "cursor");
    const auto source = plan.source;
    const auto saved = *find({entries, plan.count}, 300);
    mutable_channel(300)->last_message_id = 9999;
    mutable_channel(300)->last_read_message_id = 9999;
    ++mutable_channel(300)->revision;
    ++mutable_channel(100)->revision;
    ++guild.revision;
    check(read_scope_source_current(state, source) && read_scope_target_current(state, source, saved) &&
              saved.message_id == 3000,
          "later messages and optimistic aggregate revisions preserve immutable read boundary");
    ReadScopeSource retained_server{};
    read_scope_source(state, ReadScopeKind::guild, 1, &retained_server);
    check(read_scope_target_current(state, retained_server, saved),
          "server captured target remains structural after optimistic local unread clearing");
    ++mutable_channel(200)->generation;
    check(!read_scope_target_current(state, source, saved),
          "thread parent generation reuse rejects retained target");
    --mutable_channel(200)->generation;
    mutable_channel(200)->parent_id = 101;
    check(!read_scope_target_current(state, source, saved),
          "thread parent category movement retires category authority");
    ReadScopeSource server_source{};
    read_scope_source(state, ReadScopeKind::guild, 1, &server_source);
    check(!read_scope_target_current(state, server_source, saved),
          "server target also fences captured thread parent category movement");
    mutable_channel(200)->parent_id = 100;
    mutable_channel(300)->thread_self_joined = false;
    check(!read_scope_target_current(state, source, saved),
          "membership withdrawal rejects queued public thread target");
    mutable_channel(300)->thread_self_joined = true;
    mutable_channel(200)->effective_permissions &= ~kPermissionReadMessageHistory;
    check(!read_scope_target_current(state, source, saved),
          "resolved parent history withdrawal rejects descendant");
    mutable_channel(200)->effective_permissions |= kPermissionReadMessageHistory;
    ++mutable_channel(100)->generation;
    check(!read_scope_source_current(state, source), "category source generation reuse retires scope");
    --mutable_channel(100)->generation;
    guild.unavailable = true;
    check(!read_scope_source_current(state, source) &&
              !read_scope_plan(state, ReadScopeKind::guild, 1, epoch, {entries, 24}).valid,
          "unavailable guild fails source and plan admission closed");
    guild.unavailable = false;
    mutable_channel(100)->effective_permissions &= ~kPermissionViewChannel;
    check(!read_scope_source_current(state, source),
          "scope category VIEW withdrawal rejects retained source even with stale action bits");
    mutable_channel(100)->effective_permissions |= kPermissionViewChannel;
    ++mutable_channel(300)->generation;
    check(!read_scope_target_current(state, source, saved),
          "target generation reuse cannot receive old read intent");
    --mutable_channel(300)->generation;
    entries[0].channel_id = 888;
    check(!read_scope_plan(state, ReadScopeKind::guild, 1, epoch, {entries, 1}).valid &&
              entries[0].channel_id == 888,
          "insufficient capacity leaves the complete caller plan unchanged");
    const auto channel_before = channels[0];
    check(!read_scope_plan(state, ReadScopeKind::guild, 1, epoch,
                           {reinterpret_cast<ReadScopeTarget*>(channels), 24})
                  .valid &&
              channels[0].id == channel_before.id && channels[0].generation == channel_before.generation,
          "output aliasing Core inventory is rejected before writes");
    const u64 prior_id = channels[1].id;
    channels[1].id = channels[0].id;
    check(!read_scope_plan(state, ReadScopeKind::guild, 1, epoch, {entries, 24}).valid &&
              entries[0].channel_id == 888,
          "duplicate occupied Core identities cannot publish an ambiguous partial plan");
    channels[1].id = prior_id;
    plan = read_scope_plan(state, ReadScopeKind::guild, 1, epoch - 1, {entries, 24});
    check(plan.valid && !find({entries, plan.count}, 204)->outbound &&
              !find({entries, plan.count}, 205)->outbound,
          "clock before snowflake epoch clamps forum/media cursor to local-only zero");
    plan = read_scope_plan(state, ReadScopeKind::guild, 1, std::numeric_limits<u64>::max(), {entries, 24});
    check(plan.valid && find({entries, plan.count}, 204)->message_id ==
                            ((std::numeric_limits<u64>::max() >> 22) << 22),
          "action snowflake timestamp clamps overflow without wrapping backward");
    ReadScopeSource unchanged{};
    unchanged.id = 555;
    check(!read_scope_source(state, static_cast<ReadScopeKind>(9), 1, &unchanged) && unchanged.id == 555 &&
              !read_scope_source(state, ReadScopeKind::category, 200, &unchanged),
          "invalid scope kind and non-category cannot overwrite caller source");
    const bool empty_created = chat_state_store_upsert_channel(&state, {102, 1, 0, {}, 0, 0, 4}).ok();
    chat_state_store_recompute_guild_permissions(&state, 1);
    check(empty_created && read_scope_plan(state, ReadScopeKind::category, 102, epoch, {}).valid &&
              read_scope_plan(state, ReadScopeKind::category, 102, epoch, {}).count == 0,
          "valid empty category needs no backing output and remains distinguishable from failure");
    // Actual full inventory boundary, no heap or truncated visible projection.
    ChatStateStore large{};
    large.guilds = &guild;
    large.guild_order = guild_order;
    large.guild_count = large.guild_capacity = 1;
    large.channels = full_channels;
    large.channel_order = full_order;
    large.channel_count = kReadScopeTargetMaximum;
    large.channel_capacity = kReadScopeTargetMaximum + 1;
    for (u32 i = 0; i < kReadScopeTargetMaximum + 1; ++i) {
        full_channels[i] = {};
        auto& channel = full_channels[i];
        channel.id = i + 10000;
        channel.guild_id = 1;
        channel.generation = 1;
        channel.type = 0;
        channel.last_message_id = 123;
        channel.permissions_resolved = true;
        channel.effective_permissions = permissions;
        channel.actions = channel_action_view | channel_action_read_history;
        channel.occupied = i < kReadScopeTargetMaximum;
        full_order[i] = static_cast<u16>(i);
    }
    plan = read_scope_plan(large, ReadScopeKind::guild, 1, epoch, {full_targets, kReadScopeTargetMaximum});
    check(plan.valid && plan.count == kReadScopeTargetMaximum &&
              plan.outbound_count == kReadScopeTargetMaximum && full_targets[0].channel_id == 10000 &&
              full_targets[kReadScopeTargetMaximum - 1].channel_id == 14095,
          "whole 4096-channel inventory is admitted and sorted without visible-list truncation");
    full_channels[kReadScopeTargetMaximum].occupied = true;
    const auto before = full_targets[0];
    check(!read_scope_plan(large, ReadScopeKind::guild, 1, epoch, {full_targets, kReadScopeTargetMaximum})
                  .valid &&
              full_targets[0].channel_id == before.channel_id,
          "inventory beyond bound rejects the whole plan rather than silently truncating");
}
} // namespace maicord
