#include "maicord/app/member_list_channel_binding.hpp"
#include "maicord/model/thread_member_store.hpp"
#include "maicord/ui/member_list_scene.hpp"

namespace maicord {
namespace {
struct Fixture {
    ThreadMemberStore threads;
    u64 first_ids[512], second_ids[512];
    GuildMemberStore members;
    GuildMemberRecord member_records[512];
    u32 member_order[512];
    UserStore users;
    UserRecord user_records[512];
    u32 user_order[512];
    u8 user_names[64 * 1024];
    ChatStateStore state;
    GuildRecord guild;
    ChannelRecord channels[3];
    u16 guild_order[1], channel_order[3];
    u8 state_names[1024];
    MemberListView projection;
    MemberListRow projected_rows[768];
    u32 member_scratch[512];
    MemberListScene scene;
    MemberListRowWidget rows[128];
    UiWidget widgets[513];
    UiLayoutNode nodes[513];
};
Fixture fixture{};
bool initialize() noexcept {
    auto& f = fixture;
    if (!thread_member_store_initialize(&f.threads, {f.first_ids, 512}, {f.second_ids, 512}) ||
        !guild_member_store_initialize(&f.members, {f.member_records, 512}, {f.member_order, 512}) ||
        !user_store_initialize(&f.users, {f.user_records, 512}, {f.user_order, 512},
            {f.user_names, sizeof(f.user_names)}) ||
        !chat_state_store_initialize(&f.state, {&f.guild, 1}, {f.guild_order, 1},
            {f.channels, 3}, {f.channel_order, 3}, {f.state_names, sizeof(f.state_names)}) ||
        !member_list_view_initialize(&f.projection, {f.projected_rows, 768}, {f.member_scratch, 512}) ||
        !member_list_scene_initialize(&f.scene, {f.rows, 128}, {f.widgets, 513}, {f.nodes, 513})) return false;
    f.guild.id = 7; f.guild.occupied = true; f.guild_order[0] = 0; f.state.guild_count = 1;
    for (u32 i = 0; i < 3; ++i) {
        f.channels[i].id = 80 + i; f.channels[i].guild_id = 7;
        f.channels[i].type = i == 0 ? 0 : i == 1 ? 11 : 12;
        f.channels[i].parent_id = i ? 80 : 0; f.channels[i].occupied = true;
        f.channels[i].member_list_id = 123; f.channel_order[i] = static_cast<u16>(i);
    }
    f.state.channel_count = 3;
    return thread_member_store_select(&f.threads, 7, 81);
}
bool project() noexcept {
    auto& f = fixture;
    return member_list_view_thread(&f.projection, f.threads,
        f.members, f.users, f.state, 7, 81).ok();
}
bool show(float scroll = 0) noexcept {
    auto& f = fixture;
    return member_list_scene_update(&f.scene, &f.projection, &f.members, &f.users, &f.state,
        {0, 0, 240, 440}, scroll).ok();
}
}
u32 test_thread_member_viewport() noexcept {
    static_assert(sizeof(GuildMemberRecord) == 336);
    u32 failed = 0;
    const auto check = [&](bool valid) noexcept { if (!valid) ++failed; };
    ThreadMemberListSubscriptionRetry retry{};
    ThreadMemberListSubscriptionCommand command{};
    check(thread_member_list_subscription_select(&retry, 7, 80, true, 1'000));
    check(thread_member_list_subscription_command(retry, 1'000, &command) &&
        command.guild_id == 7 && command.thread_id == 80);
    check(thread_member_list_subscription_fail(&retry, 1'000) && retry.retry_at_ms == 1'100);
    check(thread_member_list_subscription_select(&retry, 7, 80, true, 1'020) &&
        retry.retry_at_ms == 1'100 && !thread_member_list_subscription_command(retry, 1'099, &command));
    check(thread_member_list_subscription_command(retry, 1'100, &command));
    thread_member_list_subscription_sent(&retry, command, 1'100);
    // A local scroll or repeated visible synchronization has no wire ranges
    // and must not resubscribe a whole snapshot on every frame.
    check(thread_member_list_subscription_select(&retry, 7, 80, true, 1'200) &&
        !thread_member_list_subscription_command(retry, 1'200, &command));
    check(thread_member_list_subscription_select(&retry, 7, 81, true, 1'300) &&
        thread_member_list_subscription_command(retry, 1'300, &command) && command.thread_id == 81);
    thread_member_list_subscription_sent(&retry, command, 1'300);
    // A switch to another guild explicitly clears the older interest first.
    check(thread_member_list_subscription_select(&retry, 8, 90, true, 1'400) &&
        thread_member_list_subscription_command(retry, 1'400, &command) &&
        command.guild_id == 7 && !command.thread_id);
    check(thread_member_list_subscription_fail(&retry, 1'400));
    check(thread_member_list_subscription_select(&retry, 8, 91, true, 1'450) &&
        thread_member_list_subscription_command(retry, 1'450, &command) && command.guild_id == 7);
    thread_member_list_subscription_sent(&retry, command, 1'450);
    check(thread_member_list_subscription_command(retry, 1'450, &command) &&
        command.guild_id == 8 && command.thread_id == 91);
    thread_member_list_subscription_sent(&retry, command, 1'450);
    thread_member_list_subscription_refresh(&retry, 1'500);
    check(thread_member_list_subscription_command(retry, 1'500, &command) && command.thread_id == 91);
    thread_member_list_subscription_sent(&retry, command, 1'500);
    check(thread_member_list_subscription_select(&retry, 8, 91, false, 1'600) &&
        thread_member_list_subscription_command(retry, 1'600, &command) && !command.thread_id);
    thread_member_list_subscription_sent(&retry, command, 1'600);
    check(!thread_member_list_subscription_command(retry, 1'600, &command));
    thread_member_list_subscription_complete(&retry);
    check(!retry.guild_id && !retry.sent_guild_id && !retry.retry_at_ms);
    // An older admitted command must not erase the desired current channel.
    check(thread_member_list_subscription_select(&retry, 7, 80, true, 2'000) &&
        thread_member_list_subscription_command(retry, 2'000, &command) &&
        thread_member_list_subscription_select(&retry, 7, 81, true, 2'001));
    thread_member_list_subscription_sent(&retry, command, 2'002);
    check(retry.thread_id == 81 && thread_member_list_subscription_command(retry, 2'002, &command) &&
        command.thread_id == 81);

    check(initialize());
    auto& f = fixture;
    check(member_list_channel_id(f.state, 80) == 123 && !member_list_channel_id(f.state, 81) &&
        !member_list_channel_id(f.state, 82));
    check(thread_member_store_snapshot_begin(&f.threads, 7, 81));
    for (u32 i = 0; i < 512; ++i) {
        check(thread_member_store_snapshot_append(&f.threads, 1'000 + i));
        if (i == 511) continue; // Retained membership need not have a cached profile.
        check(user_store_upsert(&f.users, 1'000 + i, utf8_literal("Member"), utf8_literal("member")).ok());
        check(guild_member_store_upsert(&f.members, f.state, 7, 1'000 + i,
            {}, {}, true, i == 1 ? GuildMemberStatus::offline : GuildMemberStatus::online).ok());
    }
    check(user_store_upsert(&f.users, 9'999, utf8_literal("Outsider"), utf8_literal("outsider")).ok());
    check(guild_member_store_upsert(&f.members, f.state, 7, 9'999, {}, {}, true,
        GuildMemberStatus::online).ok());
    check(thread_member_store_snapshot_commit(&f.threads) && project());
    check(f.projection.thread_mode && !f.projection.incomplete && f.projection.member_count == 512 &&
        f.projection.count == 513 && f.projection.count <= 768);
    for (u32 i = 0; i < f.projection.count; ++i) check(f.projection.rows[i].user_id != 9'999);
    check(show() && f.scene.count <= 128 && f.scene.source_count == 513);
    u32 member_index = ~0u;
    for (u32 i = 0; i < f.scene.count; ++i)
        if (f.scene.rows[i].kind == MemberListRowKind::member && f.scene.rows[i].user_id == 1'000) member_index = i;
    check(member_index != ~0u && member_list_scene_focus_user(&f.scene, 1'000));
    check(guild_member_store_pin_user(&f.members, 7, 1'000));
    check(guild_member_store_upsert(&f.members, f.state, 7, 10'000, {}, {}, true,
        GuildMemberStatus::online).ok());
    check(guild_member_store_find(f.members, 7, 1'000) &&
        !guild_member_store_find(f.members, 7, 1'001));
    check(!member_list_scene_row_resident(f.scene, member_index));
    check(project() && show(440 * 30.f) && f.scene.first > 250 && f.scene.count <= 128);
    check(member_list_scene_navigate(&f.scene, UiListMove::last).user_id == 1'001 &&
        member_list_scene_reveal_user(&f.scene, 1'001));
    // Snapshot replacement invalidates old native row identities before the
    // next materialization, including a known-empty replacement.
    const u32 old_count = f.scene.count;
    check(thread_member_store_snapshot_begin(&f.threads, 7, 81) &&
        thread_member_store_snapshot_commit(&f.threads));
    for (u32 i = 0; i < old_count; ++i) check(!member_list_scene_row_resident(f.scene, i));
    check(project() && !f.projection.count && !f.projection.member_count && show() && !f.scene.count);
    check(thread_member_store_snapshot_begin(&f.threads, 7, 81) &&
        thread_member_store_snapshot_append(&f.threads, 1'000) &&
        thread_member_store_snapshot_commit(&f.threads, true) && project() && f.projection.incomplete);
    check(show() && f.scene.count && member_list_scene_focus_user(&f.scene, 1'000));
    const u32 previous_revision = f.threads.revision;
    check(thread_member_store_select(&f.threads, 7, 81) && f.threads.revision == previous_revision);
    thread_member_store_reset_snapshot(&f.threads);
    check(f.threads.thread_id == 81 && !f.threads.known && project() && !f.projection.count);
    guild_member_store_reset_lists(&f.members);
    const auto* pinned_member = guild_member_store_find(f.members, 7, 1'000);
    check(pinned_member && !pinned_member->visible_pinned);
    return failed;
}
} // namespace maicord
