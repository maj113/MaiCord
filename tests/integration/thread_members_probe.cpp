#include "probe_present.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/shell_text_presentation.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/ui/member_list_scene.hpp"

namespace maicord {
namespace {
constexpr u64 kGuild = 91001, kThread = 91002;
constexpr u64 kModerator = 91010, kHelper = 91011;
constexpr u64 kUnknown = 91998, kOutsider = 91999;
bool check(bool valid, const char* failure) noexcept {
    if (!valid) log_write(LogLevel::error, "thread_members", failure);
    return valid;
}
// All membership, profile, role, layout and text backing belongs to this probe.
// The caller's shell and native renderer are borrowed only for presentation.
struct ThreadMembersFixture {
    ThreadMemberStore threads{};
    u64 first_ids[8]{}, second_ids[8]{};
    UserStore users{};
    UserRecord user_records[8]{};
    u32 user_order[8]{};
    u8 user_names[2048]{};
    GuildMemberStore members{};
    GuildMemberRecord member_records[8]{};
    u32 member_order[8]{};
    ChatStateStore state{};
    GuildRecord guilds[1]{};
    ChannelRecord channels[1]{};
    u16 guild_order[1]{}, channel_order[1]{};
    RoleRecord roles[2]{};
    PermissionOverwriteRecord overwrites[1]{};
    u8 state_names[2048]{};
    MemberListView source{};
    MemberListRow projected_rows[16]{};
    u32 scratch[8]{};
    MemberListScene scene{};
    MemberListRowWidget rows[16]{};
    UiWidget widgets[65]{};
    UiLayoutNode nodes[65]{};
};
bool snapshot(ThreadMembersFixture& f, bool partial) noexcept {
    if (!thread_member_store_snapshot_begin(&f.threads, kGuild, kThread)) return false;
    for (u64 id = 91101; id <= 91105; ++id)
        if (!thread_member_store_snapshot_append(&f.threads, id)) return false;
    return thread_member_store_snapshot_append(&f.threads, kUnknown) &&
        thread_member_store_snapshot_commit(&f.threads, partial);
}
bool initialize(ThreadMembersFixture& f) noexcept {
    if (!thread_member_store_initialize(&f.threads, {f.first_ids, 8}, {f.second_ids, 8}) ||
        !thread_member_store_select(&f.threads, kGuild, kThread) ||
        !user_store_initialize(&f.users, {f.user_records, 8}, {f.user_order, 8}, {f.user_names, 2048}) ||
        !guild_member_store_initialize(&f.members, {f.member_records, 8}, {f.member_order, 8}) ||
        !chat_state_store_initialize(&f.state, {f.guilds, 1}, {f.guild_order, 1},
            {f.channels, 1}, {f.channel_order, 1}, {f.state_names, 2048}) ||
        !chat_state_store_bind_permissions(&f.state, {f.roles, 2}, {f.overwrites, 1}) ||
        !chat_state_store_upsert_guild(&f.state, {kGuild, utf8_literal("Thread fixture"), GuildMutationKind::create, 0}).ok() ||
        !chat_state_store_upsert_role(&f.state, {kModerator, kGuild, 0, utf8_literal("Moderators"), 20, 0xE79A45, true}) ||
        !chat_state_store_upsert_role(&f.state, {kHelper, kGuild, 0, utf8_literal("Helpers"), 10, 0x69B4E8, true}) ||
        !member_list_view_initialize(&f.source, {f.projected_rows, 16}, {f.scratch, 8}) ||
        !member_list_scene_initialize(&f.scene, {f.rows, 16}, {f.widgets, 65}, {f.nodes, 65})) return false;
    const Utf8View names[] = {utf8_literal("Ada Moderator"), utf8_literal("Bela Helper"),
        utf8_literal("Cora Online"), utf8_literal("Deni Offline"), utf8_literal("Ema Named Only")};
    const Utf8View usernames[] = {utf8_literal("ada"), utf8_literal("bela"),
        utf8_literal("cora"), utf8_literal("deni"), utf8_literal("ema")};
    const GuildMemberStatus statuses[] = {GuildMemberStatus::online,
        GuildMemberStatus::idle, GuildMemberStatus::online, GuildMemberStatus::offline};
    for (u32 i = 0; i < 5; ++i) {
        if (!user_store_upsert(&f.users, 91101 + i, names[i], usernames[i]).ok()) return false;
        if (i == 4) continue; // Named membership without a guild cache slot.
        const u64 role = i == 1 ? kHelper : kModerator;
        if (!guild_member_store_upsert(&f.members, f.state, kGuild, 91101 + i, {},
            i == 2 ? ConstSlice<u64>{} : ConstSlice<u64>{&role, 1}, true, statuses[i]).ok()) return false;
    }
    return user_store_upsert(&f.users, kOutsider, utf8_literal("Guild Outsider"), utf8_literal("outsider")).ok() &&
        guild_member_store_upsert(&f.members, f.state, kGuild, kOutsider, {}, {}, true,
            GuildMemberStatus::online).ok() && snapshot(f, false);
}
bool text_contract(const D3D11Renderer& renderer, const ThreadMembersFixture& f, bool partial) noexcept {
    UiTextCommand commands[kMaximumShellChromeTextCommands]{};
    char bytes[8192]{};
    UiTextCommandStream stream{};
    if (!ui_text_command_stream_initialize(&stream, {commands, kMaximumShellChromeTextCommands}, {bytes, 8192}) ||
        !shell_text_presentation_build(*renderer.ui_scene, *renderer.chat_state,
            *renderer.navigation_view, kDefaultTheme, *renderer.shell_navigation_scene,
            renderer.message_composer_scene, &f.scene, &stream)) return check(false, "text_build_failed");
    const Utf8View groups[] = {
        partial ? utf8_literal("MODERATORS \xE2\x80\x94 1 (partial)") : utf8_literal("MODERATORS \xE2\x80\x94 1"),
        partial ? utf8_literal("HELPERS \xE2\x80\x94 1 (partial)") : utf8_literal("HELPERS \xE2\x80\x94 1"),
        partial ? utf8_literal("ONLINE \xE2\x80\x94 1 (partial)") : utf8_literal("ONLINE \xE2\x80\x94 1"),
        partial ? utf8_literal("OFFLINE \xE2\x80\x94 2 (partial)") : utf8_literal("OFFLINE \xE2\x80\x94 2")};
    u32 group_mask = 0, named_members = 0;
    for (u32 i = 0; i < stream.count; ++i) {
        const auto& command = commands[i];
        if (command.owner.manager != &f.scene.widgets) continue;
        for (u32 j = 0; j < 4; ++j) if (utf8_equal(command.text, groups[j])) group_mask |= 1u << j;
        if (utf8_equal(command.text, utf8_literal("Guild Outsider")) ||
            utf8_equal(command.text, utf8_literal("Unknown user"))) return check(false, "nonmember_or_unknown_text");
        for (u32 j = 0; j < 5; ++j) {
            const auto* user = user_store_find(f.users, 91101 + j);
            if (user && utf8_equal(command.text, user_store_name(f.users, *user))) ++named_members;
        }
    }
    return check(group_mask == 15, "group_text_incorrect") && check(named_members == 5, "named_member_text_incorrect");
}
bool scene_contract(const ThreadMembersFixture& f) noexcept {
    if (!check(f.source.thread_mode && !f.source.authoritative && f.source.member_count == 6 &&
        f.source.count == 9 && f.scene.count == 9, "projection_counts_incorrect")) return false;
    u32 next = 0;
    for (u32 i = 0; i < f.scene.count; ++i) {
        const auto& row = f.scene.rows[i];
        if (row.kind != MemberListRowKind::member) continue;
        if (!check(row.user_id == 91101 + next++, "member_order_or_scope_incorrect") ||
            !check(member_list_scene_row_resident(f.scene, i), "named_member_not_resident")) return false;
        const auto* layout = member_list_scene_layout_node(f.scene, row.widget);
        if (!layout || !check(member_list_scene_hit_test(f.scene,
            (layout->rectangle.left + layout->rectangle.right) * .5f,
            (layout->rectangle.top + layout->rectangle.bottom) * .5f).user_id == row.user_id,
            "named_member_hit_incorrect")) return false;
        if (row.user_id == 91105 && !check(row.source_row.member_slot == ~0u,
            "named_only_member_has_guild_slot")) return false;
    }
    return check(next == 5 && thread_member_store_contains(f.threads, kUnknown) &&
        !thread_member_store_contains(f.threads, kOutsider), "identity_scope_incorrect");
}
} // namespace

// Caller supplies a native guild shell with the member pane visible. Captures
// concern the owned membership pane; surrounding chrome belongs to the caller.
bool thread_members_probe(D3D11Renderer& renderer, UiTextMeasureCache& measure,
    bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!capture || !renderer.ui_scene || !renderer.chat_state ||
        !renderer.navigation_view || !renderer.shell_navigation_scene) return false;
    const auto* pane = ui_scene_resolve(*renderer.ui_scene,
        ui_scene_find(*renderer.ui_scene, UiSemantic::member_sidebar));
    if (!pane || !ui_rect_has_area(pane->rectangle)) return false;
    ThreadMembersFixture f{};
    const auto* previous = renderer.member_list_scene;
    const UiRect previous_scrollbar = renderer.scrollbar_viewport;
    bool valid = check(initialize(f), "fixture_initialize_failed");
    f.scene.measure = &measure;
    for (u32 pass = 0; valid && pass < 2; ++pass) {
        const bool partial = pass != 0;
        valid = (!partial || snapshot(f, true)) &&
            member_list_view_thread(&f.source, f.threads, f.members, f.users, f.state, kGuild, kThread).ok() &&
            member_list_scene_update(&f.scene, &f.source, &f.members, &f.users, &f.state, pane->rectangle, 0).ok() &&
            check(f.source.incomplete == partial, "partial_state_incorrect") && scene_contract(f) && text_contract(renderer, f, partial);
        if (valid) {
            renderer.scrollbar_viewport = f.scene.viewport;
            valid = renderer_bind_member_list_scene(&renderer, &f.scene) &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
                capture(partial ? L"thread-members-partial.bmp" : L"thread-members-complete.bmp");
        }
    }
    // Remove all borrowed fixture pointers while their backing still exists.
    renderer.member_list_scene = previous;
    renderer.scrollbar_viewport = previous_scrollbar;
    const bool restored = renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    log_write(valid && restored ? LogLevel::info : LogLevel::error, "probe",
        valid && restored ? "thread_members_native_hardware_passed" : "thread_members_native_hardware_failed");
    return valid && restored;
}
} // namespace maicord
