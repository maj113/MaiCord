// Role manager: tri-state permission maths and the tri-state switch, role
// reorder requests and hierarchy, View Server as Role recomputation, role
// locks and the editor's unsaved state. Synthetic fixtures only.
#include "maicord/model/guild_settings_model.hpp"
#include "maicord/model/permission_compute.hpp"
#include "maicord/model/permission_grid.hpp"
#include "maicord/protocol/rest.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace role_manager_test {

u32 failures;
void check(bool success, const char* message) noexcept {
    test_check(&failures, success, message);
}

constexpr u64 kGuild = 100, kOwner = 1, kSelf = 7;
constexpr u64 kChannelOpen = 500, kChannelStaff = 501, kChannelMuted = 502;
constexpr u64 kRoleTop = 10, kRoleSelf = 11, kRoleStaff = 12, kRoleLow = 13, kRoleBot = 14;

struct Model {
    ChatStateStore state;
    GuildRecord guilds[2];
    ChannelRecord channels[8];
    RoleRecord roles[16];
    PermissionOverwriteRecord overwrites[16];
    u16 guild_order[2], channel_order[8];
    u8 names[4096];
};
Model model;

bool add_role(u64 id, PermissionSet permissions, u32 position, bool managed = false) noexcept {
    RoleInput input{id, kGuild, permissions, utf8_literal("role"), position};
    input.managed = managed;
    return chat_state_store_upsert_role(&model.state, input);
}
bool overwrite(u64 channel, u64 id, PermissionSet allow, PermissionSet deny) noexcept {
    return chat_state_store_append_channel_overwrite(&model.state, channel, {id, allow, deny, 0});
}

// Self holds kRoleSelf (position 6) below kRoleTop. #staff hides from
// @everyone and shows to kRoleStaff; #muted denies kRoleLow sending.
bool build(u64 owner, PermissionSet self_permissions) noexcept {
    clear_bytes(&model, sizeof(model));
    auto& state = model.state;
    if (!chat_state_store_initialize(&state, {model.guilds, 2}, {model.guild_order, 2}, {model.channels, 8},
                                     {model.channel_order, 8}, {model.names, sizeof(model.names)}) ||
        !chat_state_store_bind_permissions(&state, {model.roles, 16}, {model.overwrites, 16}))
        return false;
    state.self_user_id = kSelf;
    state.self_mfa = SelfMfaState::enabled;
    if (!chat_state_store_upsert_guild(&state, {kGuild, utf8_literal("Guild"), GuildMutationKind::create, owner})
             .ok() ||
        !chat_state_store_upsert_channel(&state, {kChannelOpen, kGuild, 0, utf8_literal("open"), 0, 0, 0}).ok() ||
        !chat_state_store_upsert_channel(&state, {kChannelStaff, kGuild, 0, utf8_literal("staff"), 1, 0, 0}).ok() ||
        !chat_state_store_upsert_channel(&state, {kChannelMuted, kGuild, 0, utf8_literal("muted"), 2, 0, 0}).ok() ||
        !add_role(kGuild, kPermissionViewChannel | kPermissionSendMessages | kPermissionReadMessageHistory, 0) ||
        !add_role(kRoleTop, 0, 9) || !add_role(kRoleSelf, self_permissions, 6) ||
        !add_role(kRoleStaff, kPermissionManageMessages, 4) || !add_role(kRoleLow, 0, 2) ||
        !add_role(kRoleBot, kPermissionKickMembers, 3, true) ||
        !overwrite(kChannelStaff, kGuild, 0, kPermissionViewChannel) ||
        !overwrite(kChannelStaff, kRoleStaff, kPermissionViewChannel, 0) ||
        !overwrite(kChannelMuted, kRoleLow, 0, kPermissionSendMessages))
        return false;
    const u64 self_roles[] = {kRoleSelf};
    if (!chat_state_store_set_self_roles(&state, kGuild, owner, kSelf, {self_roles, 1}))
        return false;
    chat_state_store_recompute_guild_permissions(&state, kGuild);
    return true;
}
PermissionSet channel_permissions(u64 id) noexcept {
    const auto* channel = chat_state_store_find_channel(model.state, id);
    return channel && channel->permissions_resolved ? channel->effective_permissions : 0;
}

// Allow / neutral / deny bits of one override.
void test_tristate_bits() noexcept {
    constexpr PermissionSet kSend = kPermissionSendMessages, kAttach = kPermissionAttachFiles,
                            kOther = kPermissionEmbedLinks;
    PermissionSet allow = kOther, deny = kOther << 1;
    const PermissionTriState cycle[]{PermissionTriState::deny, PermissionTriState::neutral, PermissionTriState::allow,
                                     PermissionTriState::deny};
    bool exact = true;
    for (const auto value : cycle) {
        permission_tri_set(&allow, &deny, kSend, value);
        exact = exact && permission_tri_get(allow, deny, kSend) == value && (allow & kOther) &&
                (deny & (kOther << 1)) && !((allow & deny) & kSend) &&
                ((allow & kSend) != 0) == (value == PermissionTriState::allow) &&
                ((deny & kSend) != 0) == (value == PermissionTriState::deny);
    }
    check(exact, "tri-state: deny, neutral and allow each set one side only and keep other bits");
    check(permission_tri_get(kSend, kSend, kSend) == PermissionTriState::allow &&
              permission_tri_get(0, 0, kAttach) == PermissionTriState::neutral,
          "tri-state: allow wins over a malformed pair; neither bit is neutral");
    PermissionOverwriteDraft draft{};
    check(permission_overwrite_draft_add(&draft, kRoleLow, 0) &&
              permission_overwrite_draft_set(&draft, kRoleLow, kAttach, PermissionTriState::allow) &&
              permission_overwrite_draft_set(&draft, kRoleLow, kSend, PermissionTriState::deny) &&
              draft.entries[0].allow == kAttach && draft.entries[0].deny == kSend &&
              permission_overwrite_draft_set(&draft, kRoleLow, kSend, PermissionTriState::neutral) &&
              draft.entries[0].deny == 0 && draft.entries[0].allow == kAttach,
          "tri-state: an override draft keeps allow and deny masks in step");
    // Resolution: @everyone, then the roles' aggregate, then the member.
    RoleRecord roles[2]{};
    roles[0].id = kGuild, roles[0].guild_id = kGuild, roles[0].permissions = kPermissionViewChannel | kSend;
    roles[1].id = kRoleLow, roles[1].guild_id = kGuild, roles[1].position = 1;
    const PermissionGuild guild{kGuild, kOwner, {roles, 2}, false};
    const u64 member_roles[]{kRoleLow};
    const PermissionMember member{kSelf, {member_roles, 1}, 0};
    const PermissionOverwriteRecord deny_everyone[]{{kGuild, 0, kSend, 0}};
    const PermissionOverwriteRecord role_allows[]{{kGuild, 0, kSend, 0}, {kRoleLow, kSend, 0, 0}};
    const PermissionOverwriteRecord member_denies[]{{kRoleLow, kSend, 0, 0}, {kSelf, 0, kSend, 1}};
    check(!(permission_compute(guild, member, {deny_everyone, 1}, {}) & kSend) &&
              (permission_compute(guild, member, {role_allows, 2}, {}) & kSend) &&
              !(permission_compute(guild, member, {member_denies, 2}, {}) & kSend) &&
              (permission_compute(guild, member, {}, {}) & kSend),
          "tri-state: neutral inherits, a role allow beats @everyone deny, a member deny beats role allows");
}

// The joined switch: three segments, every point of the group hits one.
void test_tristate_control() noexcept {
    const auto tokens = ui_settings_tokens(kDefaultTheme);
    const float width = ui_tristate_width(tokens), segment = tokens.metric.tristate_segment;
    const UiRect group{100, 40, 100 + width, 40 + tokens.metric.segment_size};
    bool contiguous = true;
    for (u32 i = 0; i < kUiTristateCount; ++i) {
        const auto option = ui_tristate_option(tokens, group, i);
        contiguous = contiguous && option.left == group.left + segment * i && option.right == option.left + segment &&
                     option.top == group.top && option.bottom == group.bottom;
    }
    check(width == 96 && contiguous && !ui_rect_has_area(ui_tristate_option(tokens, group, 3)),
          "tri-state control: a 96 DIP group of three joined 32 DIP segments");
    const float y = (group.top + group.bottom) * .5f;
    check(ui_tristate_hit(tokens, group, group.left, y) == 0 &&
              ui_tristate_hit(tokens, group, group.left + segment - .01f, y) == 0 &&
              ui_tristate_hit(tokens, group, group.left + segment, y) == 1 &&
              ui_tristate_hit(tokens, group, group.left + segment * 2 + 1, y) == 2 &&
              ui_tristate_hit(tokens, group, group.right - .01f, group.bottom - .01f) == 2 &&
              ui_tristate_hit(tokens, group, group.left - .01f, y) == ~0u &&
              ui_tristate_hit(tokens, group, group.right + 1, y) == ~0u &&
              ui_tristate_hit(tokens, group, group.left + 1, group.top - 1) == ~0u,
          "tri-state control: segment boundaries hit exactly; points outside the group miss");
    PaintStream paint{};
    bool painted = paint_stream_initialize(&paint, 64);
    u32 counts[kUiTristateCount]{}, clip_counts[kUiTristateCount]{};
    for (u32 selected = 0; painted && selected < kUiTristateCount; ++selected) {
        painted = paint_stream_reset(&paint, 1000, 1000) &&
                  ui_tristate_paint(&paint, tokens, group, selected, ~0u, {true, false, false, false});
        counts[selected] = paint.count;
        // The selected end segment fills the inner shape, clipped to its span.
        const u32 clips = paint.clip_count;
        const auto option = ui_tristate_option(tokens, group, selected);
        const auto& fill = paint.quads[selected == 0 ? 1 : selected == 1 ? 2 : 3];
        if (painted && selected != 1) {
            const auto& clip = paint.clips[clips - 1];
            painted = fill.left == group.left + 1 && fill.right == group.right - 1 && fill.top == group.top + 1 &&
                      fill.radius == ui_tristate_radius(tokens) - 1 &&
                      clip.left == (selected == 0 ? group.left + 1 : option.left) &&
                      clip.right == (selected == 2 ? group.right - 1 : option.right);
        } else if (painted)
            painted = fill.left == option.left && fill.right == option.right && fill.radius == 0;
        clip_counts[selected] = clips;
    }
    check(painted && counts[0] == counts[1] && counts[1] == counts[2] && counts[0] == 5 &&
              clip_counts[0] == clip_counts[2] && clip_counts[1] + 1 == clip_counts[0],
          "tri-state control: one group, one concentric selected fill and three glyphs per state; only end "
          "segments clip");
    painted = painted && paint_stream_reset(&paint, 1000, 1000) &&
              ui_tristate_paint(&paint, tokens, group, 1, 2, {true, true, false, true});
    check(painted && paint.count == 7, "tri-state control: hover adds a segment fill and focus a ring");
    paint_stream_release(&paint);

    // Scene transitions: the pointer picks the segment under it, keys step
    // deny → neutral → allow → deny.
    static UiSettingsScene scene{};
    static UiPage page{};
    static UiPageRegistry registry{};
    const Utf8View states[]{utf8_literal("Deny"), utf8_literal("Neutral"), utf8_literal("Allow")};
    UiSettingsControl controls[]{
        {1, utf8_literal("Send Messages"), {}, UiSettingsControlKind::segments, true, false, {states, 3}, 0, {}, nullptr}};
    UiSettingsControlPresentation looks[]{{UiSettingsVariant::standard, ui_settings_presentation_tristate,
                                           kUiSettingsNoDefault}};
    UiSettingsPageContent content{utf8_literal("Permissions"), {}, {controls, 1}, 1, kUiSettingsPageFormat};
    content.presentation = looks;
    const UiPageKey key{7, 1};
    const UiSettingsTarget target{UiSettingsTargetKind::control, key, 1};
    const bool ready = ui_page_registry_initialize(&registry, {&page, 1}) &&
                       ui_page_registry_register(&registry, {key, utf8_literal("Permissions"), &content}).ok() &&
                       ui_settings_scene_update(&scene, &registry, {0, 0, 1280, 900});
    UiRect rect[kUiTristateCount]{};
    bool geometry = ready;
    for (u32 i = 0; geometry && i < kUiTristateCount; ++i)
        geometry = ui_settings_scene_choice_rectangle(scene, target, i, &rect[i]) &&
                   ui_rect_width(rect[i]) == segment && (!i || rect[i].left == rect[i - 1].right);
    UiSettingsActivation hit{};
    bool pointer = geometry;
    for (u32 i = 0; pointer && i < kUiTristateCount; ++i)
        pointer = ui_settings_scene_option_pointer(scene, target, (rect[i].left + rect[i].right) * .5f,
                                                   (rect[i].top + rect[i].bottom) * .5f, &hit) &&
                  hit.selected_choice == i;
    check(pointer && !ui_settings_scene_option_pointer(scene, target, rect[2].right + 4, rect[2].top + 2, &hit),
          "tri-state control: scene geometry is the joined group and the pointer selects the segment under it");
    u32 sequence[4]{};
    bool keys = ready;
    for (u32 i = 0; keys && i < 4; ++i) {
        UiSettingsActivation step{};
        keys = ui_settings_scene_activation(scene, target, false, &step);
        sequence[i] = step.selected_choice;
        controls[0].selected_choice = step.selected_choice;
        ++content.revision;
        keys = keys && ui_settings_scene_update(&scene, &registry, {0, 0, 1280, 900});
    }
    check(keys && sequence[0] == 1 && sequence[1] == 2 && sequence[2] == 0 && sequence[3] == 1,
          "tri-state control: activation steps deny, neutral, allow and wraps");
    ui_settings_scene_release(&scene);
}

// Role reorder: PATCH /guilds/{g}/roles, moves limited by the hierarchy.
void test_reorder() noexcept {
    check(build(kOwner, kPermissionManageRoles), "reorder: fixture builds");
    const auto access = guild_settings_access(model.state, kGuild);
    const RoleRecord* order[8];
    const u32 count = guild_settings_sorted_roles(model.state, kGuild, {order, 8});
    // top, self, staff, bot, low, @everyone
    check(count == 6 && order[0]->id == kRoleTop && order[1]->id == kRoleSelf && order[5]->id == kGuild,
          "reorder: roles sort highest first with @everyone last");
    check(!guild_settings_role_move(access, {order, count}, 2, 1) &&
              !guild_settings_role_move(access, {order, count}, 1, 3) &&
              !guild_settings_role_move(access, {order, count}, 0, 2) &&
              !guild_settings_role_move(access, {order, count}, 4, 5) && order[2]->id == kRoleStaff,
          "reorder: nothing moves to or from the account's highest role and above, nor below @everyone");
    check(guild_settings_role_move(access, {order, count}, 4, 2) && order[2]->id == kRoleLow &&
              order[3]->id == kRoleStaff && order[4]->id == kRoleBot,
          "reorder: a role below the account moves up to just under it");
    GuildAdminPosition positions[8];
    const u32 written = guild_settings_role_positions({order, count}, kGuild, {positions, 8});
    GuildAdminRequest request{};
    request.operation = GuildAdminOperation::role_reorder;
    request.guild_id = kGuild;
    request.positions = {positions, written};
    u8 target[256], body[512];
    const auto path = guild_admin_write_target(request, {target, sizeof(target)});
    const auto json = guild_admin_write_body(request, {body, sizeof(body)});
    constexpr char kPath[] = "/api/v10/guilds/100/roles";
    constexpr char kBody[] = R"([{"id":"13","position":4},{"id":"12","position":3},{"id":"14","position":2}])";
    check(guild_admin_method(request.operation) == RestMethod::patch && guild_admin_request_valid(request) &&
              path.ok() && path.bytes == sizeof(kPath) - 1 && bytes_equal(target, kPath, path.bytes) && json.ok() &&
              json.bytes == sizeof(kBody) - 1 && bytes_equal(body, kBody, json.bytes) &&
              !guild_admin_reason_header(request),
          "reorder: PATCH /guilds/{id}/roles carries only the moved roles' positions, without a reason header");
    // The Roles list's unsaved state follows the draft order.
    u64 ids[8];
    for (u32 i = 0; i < count; ++i)
        ids[i] = order[i]->id;
    check(guild_settings_order_changed(model.state, kGuild, {ids, count}), "save bar: a moved role is unsaved");
    check(guild_settings_role_move(access, {order, count}, 2, 4), "save bar: the role moves back");
    for (u32 i = 0; i < count; ++i)
        ids[i] = order[i]->id;
    check(!guild_settings_order_changed(model.state, kGuild, {ids, count}),
          "save bar: moving back to the stored order clears it");
    check(chat_state_store_remove_role(&model.state, kGuild, kRoleLow) &&
              !guild_settings_order_changed(model.state, kGuild, {ids, count}) &&
              guild_settings_order_changed(model.state, kGuild, {ids, count - 2}),
          "save bar: a role deleted elsewhere drops out of the draft; a missing stored role is a change");
}

// View Server as Role: local recomputation, owner exemption dropped.
void test_view_as() noexcept {
    check(build(kSelf, 0), "view as: owner fixture builds");
    auto& state = model.state;
    const PermissionSet owner_base = state.guilds[0].base_permissions;
    check(owner_base == kPermissionAll && (channel_permissions(kChannelStaff) & kPermissionViewChannel) &&
              !chat_state_store_viewing_as(state, kGuild),
          "view as: the owner starts with every permission and channel");
    const u64 low[]{kRoleLow};
    check(chat_state_store_view_as_roles(&state, kGuild, {low, 1}) && chat_state_store_viewing_as(state, kGuild),
          "view as: a view starts");
    const auto* guild = chat_state_store_find_guild(state, kGuild);
    check(guild && guild->base_permissions ==
                       (kPermissionViewChannel | kPermissionSendMessages | kPermissionReadMessageHistory) &&
              !(channel_permissions(kChannelStaff) & kPermissionViewChannel) &&
              (channel_permissions(kChannelOpen) & kPermissionSendMessages) &&
              !(channel_permissions(kChannelMuted) & kPermissionSendMessages) &&
              (channel_permissions(kChannelMuted) & kPermissionViewChannel),
          "view as: the owner sees only what the role grants, overrides included");
    u64 viewed[4];
    check(chat_state_store_view_roles(state, kGuild, {viewed, 4}) == 1 && viewed[0] == kRoleLow,
          "view as: the viewed roles exclude @everyone");
    const u64 staff[]{kRoleStaff, 999, kGuild};
    check(chat_state_store_view_as_roles(&state, kGuild, {staff, 3}) &&
              (channel_permissions(kChannelStaff) & kPermissionViewChannel) &&
              (channel_permissions(kChannelMuted) & kPermissionSendMessages) &&
              chat_state_store_view_roles(state, kGuild, {viewed, 4}) == 1 && viewed[0] == kRoleStaff,
          "view as: switching roles recomputes; unknown ids and @everyone are ignored");
    check(chat_state_store_view_as_roles(&state, kGuild, {}) &&
              chat_state_store_view_roles(state, kGuild, {viewed, 4}) == 0 &&
              !(channel_permissions(kChannelStaff) & kPermissionViewChannel),
          "view as: an empty set views as @everyone");
    // Gateway changes keep the view: the account's roles and a viewed role's
    // permissions update underneath it.
    check(chat_state_store_view_as_roles(&state, kGuild, {staff, 1}) &&
              add_role(kRoleStaff, kPermissionManageMessages | kPermissionBanMembers, 4),
          "view as: a viewed role updates");
    chat_state_store_recompute_guild_permissions(&state, kGuild);
    check(chat_state_store_viewing_as(state, kGuild) &&
              (chat_state_store_find_guild(state, kGuild)->base_permissions & kPermissionBanMembers),
          "view as: updates to a viewed role apply to the view");
    check(chat_state_store_remove_role(&state, kGuild, kRoleStaff), "view as: a viewed role is deleted");
    chat_state_store_recompute_guild_permissions(&state, kGuild);
    check(chat_state_store_viewing_as(state, kGuild) && chat_state_store_view_roles(state, kGuild, {viewed, 4}) == 0 &&
              !(channel_permissions(kChannelStaff) & kPermissionViewChannel),
          "view as: a deleted role drops out and the view continues as @everyone");
    check(chat_state_store_end_view_as(&state, kGuild) && !chat_state_store_viewing_as(state, kGuild) &&
              chat_state_store_find_guild(state, kGuild)->base_permissions == owner_base &&
              (channel_permissions(kChannelStaff) & kPermissionViewChannel) &&
              !chat_state_store_end_view_as(&state, kGuild),
          "view as: ending restores the account's own permissions once");
    // The selection leaves a channel the view hides.
    check(build(kOwner, kPermissionAdministrator) &&
              chat_state_store_select_channel(&model.state, kChannelStaff).ok() &&
              model.state.selected_channel_id == kChannelStaff,
          "view as: an administrator fixture opens #staff");
    check(chat_state_store_view_as_roles(&model.state, kGuild, {low, 1}) &&
              model.state.selected_channel_id != kChannelStaff,
          "view as: a channel the viewed role cannot see is left");
    check(!chat_state_store_view_as_roles(&model.state, 999, {low, 1}) && !chat_state_store_end_view_as(nullptr, kGuild),
          "view as: unknown guilds are refused");
}

// Locks, View as permission, and the editor's draft guards.
void test_locks_and_drafts() noexcept {
    check(build(kOwner, kPermissionManageRoles | kPermissionManageGuild | kPermissionKickMembers),
          "locks: manager fixture builds");
    auto access = guild_settings_access(model.state, kGuild);
    const auto* top = chat_state_store_find_role(model.state, kGuild, kRoleTop);
    const auto* self = chat_state_store_find_role(model.state, kGuild, kRoleSelf);
    const auto* low = chat_state_store_find_role(model.state, kGuild, kRoleLow);
    const auto* everyone = chat_state_store_find_role(model.state, kGuild, kGuild);
    check(top && self && low && everyone && guild_settings_role_lock(access, *top) == GuildRoleLock::above &&
              guild_settings_role_lock(access, *self) == GuildRoleLock::highest &&
              guild_settings_role_lock(access, *low) == GuildRoleLock::none,
          "locks: roles at and above the account's highest are locked with their reason");
    check(guild_settings_can_view_as_role(access, *low) && guild_settings_can_view_as_role(access, *self) &&
              guild_settings_can_view_as_role(access, *everyone) && !guild_settings_can_view_as_role(access, *top),
          "view as: Manage Server and Manage Roles view roles up to the account's highest");
    // The self role: Kick Members is the account's own through this role.
    GuildRoleDraft draft = guild_role_draft_from(model.state, *self);
    check(guild_role_permission_lock(model.state, access, draft, kPermissionKickMembers) == GuildPermissionLock::self &&
              guild_role_permission_lock(model.state, access, draft, kPermissionBanMembers) ==
                  GuildPermissionLock::missing &&
              !guild_role_can_clear(model.state, access, draft),
          "locks: removing the account's own permission, granting a missing one or clearing them is refused");
    GuildRoleDraft other = guild_role_draft_from(model.state, *low);
    check(guild_role_toggle(&other, access, kPermissionKickMembers, true) &&
              guild_role_permission_lock(model.state, access, other, kPermissionKickMembers) ==
                  GuildPermissionLock::none &&
              guild_role_can_clear(model.state, access, other),
          "locks: another role's permissions toggle and clear freely");
    const auto base = guild_role_draft_from(model.state, *low);
    check(!guild_role_draft_equal(base, other), "save bar: a toggled permission is unsaved");
    check(guild_role_toggle(&other, access, kPermissionKickMembers, false) && guild_role_draft_equal(base, other),
          "save bar: toggling it back clears the unsaved state");
    other.permissions = 0;
    RolePatch patch{};
    check(guild_role_draft_equal(base, other) && !guild_role_patch(base, other, &patch),
          "save bar: clearing an empty role changes nothing");
    // A role the account lacks: no permission at all for Manage Roles.
    check(build(kOwner, kPermissionKickMembers), "locks: member fixture builds");
    access = guild_settings_access(model.state, kGuild);
    low = chat_state_store_find_role(model.state, kGuild, kRoleLow);
    check(low && guild_settings_role_lock(access, *low) == GuildRoleLock::no_permission &&
              !guild_settings_can_view_as_role(access, *low),
          "locks: without Manage Roles every role is locked and View as is unavailable");
    // The owner may grant, remove and view as anything.
    check(build(kSelf, 0), "locks: owner fixture builds");
    access = guild_settings_access(model.state, kGuild);
    top = chat_state_store_find_role(model.state, kGuild, kRoleTop);
    draft = guild_role_draft_from(model.state, *top);
    check(top && guild_settings_can_view_as_role(access, *top) &&
              guild_role_permission_lock(model.state, access, draft, kPermissionAdministrator) ==
                  GuildPermissionLock::none,
          "locks: the owner is never locked");
}

} // namespace role_manager_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_role_manager_entry() noexcept {
    using namespace maicord::role_manager_test;
    test_tristate_bits();
    test_tristate_control();
    test_reorder();
    test_view_as();
    test_locks_and_drafts();
    constexpr char pass[] = "PASS role manager\n";
    constexpr char fail[] = "FAIL role manager\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}
