#include "maicord/model/channel_settings_model.hpp"
#include "maicord/model/guild_ban_store.hpp"
#include "maicord/model/guild_settings_model.hpp"
#include "maicord/model/permission_grid.hpp"
#include "maicord/protocol/guild_assets.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"

namespace maicord {
namespace guild_settings_test {

u32 failures;
void check(bool success, const char* message) noexcept {
    test_check(&failures, success, message);
}

constexpr u64 kGuild = 100, kOwner = 1, kSelf = 7, kChannel = 500, kCategory = 501;
constexpr u64 kRoleTop = 10, kRoleSelf = 11, kRoleMid = 12, kRoleLow = 13, kRoleBot = 14;

struct Model {
    ChatStateStore state;
    GuildRecord guilds[2];
    ChannelRecord channels[4];
    RoleRecord roles[16];
    PermissionOverwriteRecord overwrites[16];
    u16 guild_order[2], channel_order[4];
    u8 names[4096];
};
Model model;

bool add_role(u64 id, PermissionSet permissions, u32 position, bool managed = false) noexcept {
    RoleInput input{id, kGuild, permissions, utf8_literal("role"), position};
    input.managed = managed;
    return chat_state_store_upsert_role(&model.state, input);
}

// Self holds kRoleSelf with the given permissions, below kRoleTop and above the rest.
bool build(u64 owner, PermissionSet self_permissions) noexcept {
    clear_bytes(&model, sizeof(model));
    auto& state = model.state;
    if (!chat_state_store_initialize(&state, {model.guilds, 2}, {model.guild_order, 2}, {model.channels, 4},
                                        {model.channel_order, 4}, {model.names, sizeof(model.names)}) ||
        !chat_state_store_bind_permissions(&state, {model.roles, 16}, {model.overwrites, 16}))
        return false;
    state.self_user_id = kSelf;
    state.self_mfa = SelfMfaState::enabled;
    if (!chat_state_store_upsert_guild(&state, {kGuild, utf8_literal("Guild"), GuildMutationKind::create, owner})
             .ok() ||
        !chat_state_store_upsert_channel(&state, {kCategory, kGuild, 0, utf8_literal("cat"), 0, 0, 4}).ok() ||
        !chat_state_store_upsert_channel(&state, {kChannel, kGuild, kCategory, utf8_literal("chat"), 1, 0, 0})
             .ok() ||
        !add_role(kGuild, kPermissionViewChannel | kPermissionSendMessages, 0) || !add_role(kRoleTop, 0, 9) ||
        !add_role(kRoleSelf, self_permissions, 6) || !add_role(kRoleMid, kPermissionAttachFiles, 4) ||
        !add_role(kRoleLow, 0, 2) || !add_role(kRoleBot, 0, 3, true))
        return false;
    const u64 self_roles[] = {kRoleSelf};
    if (!chat_state_store_set_self_roles(&state, kGuild, owner, kSelf, {self_roles, 1}))
        return false;
    chat_state_store_recompute_guild_permissions(&state, kGuild);
    return true;
}

bool body_equals(const GuildAdminRequest& request, const char* expected) noexcept {
    if (!guild_admin_request_valid(request))
        return false;
    u8 body[2048];
    const auto written = guild_admin_write_body(request, {body, sizeof(body)});
    u32 length = 0;
    while (expected[length])
        ++length;
    return written.ok() && written.bytes == length && bytes_equal(body, expected, length);
}

void test_access() noexcept {
    check(build(kOwner, 0), "access: fixture builds");
    auto access = guild_settings_access(model.state, kGuild);
    check(!access.owner && access.resolved && access.sections == 0,
          "access: a member without administrative permissions gets no Server Settings sections");
    check(build(kOwner, kPermissionManageRoles), "access: manage roles fixture");
    access = guild_settings_access(model.state, kGuild);
    constexpr u32 always = guild_settings_profile | guild_settings_engagement;
    check(access.sections == (always | guild_settings_roles | guild_settings_members) &&
              access.top_role_id == kRoleSelf && !guild_settings_can_manage_guild(access),
          "access: Manage Roles opens Roles and Members beside the read-only Profile and Engagement");
    check(build(kOwner, kPermissionBanMembers | kPermissionViewAuditLog | kPermissionManageGuild),
          "access: moderator fixture");
    access = guild_settings_access(model.state, kGuild);
    check(access.sections == (always | guild_settings_invites | guild_settings_members | guild_settings_bans |
                              guild_settings_audit_log | guild_settings_safety) &&
              guild_settings_can_manage_guild(access) && guild_settings_can_unban(access) &&
              !access.manage_expressions && !guild_settings_can_delete(access) && !guild_settings_can_set_mfa(access),
          "access: Manage Server, Ban Members and View Audit Log open their sections; owner actions stay closed");
    check(build(kOwner, kPermissionCreateExpressions), "access: create expressions fixture");
    access = guild_settings_access(model.state, kGuild);
    check(access.sections == (always | guild_settings_expressions) && !access.manage_expressions,
          "access: Create Expressions opens Emoji and Stickers for uploads only");
    check(build(kOwner, kPermissionManageExpressions), "access: manage expressions fixture");
    access = guild_settings_access(model.state, kGuild);
    check(access.sections == (always | guild_settings_expressions) && access.manage_expressions,
          "access: Manage Expressions (bit 30) edits and deletes any expression");
    check(build(kSelf, 0), "access: owner fixture");
    access = guild_settings_access(model.state, kGuild);
    check(access.owner && access.sections == 0x1ff && guild_settings_grantable(access) == kPermissionKnownMask &&
              access.manage_expressions && guild_settings_can_delete(access) && guild_settings_can_set_mfa(access),
          "access: the owner sees every section, may grant every known permission, delete and set 2FA");
    check(build(kOwner, kPermissionManageRoles), "access: two-factor fixture");
    GuildSettings settings{};
    settings.known = guild_settings_mfa_level;
    settings.mfa_level = 1;
    model.state.self_mfa = SelfMfaState::disabled;
    check(chat_state_store_set_guild_settings(&model.state, kGuild, settings) &&
              guild_settings_access(model.state, kGuild).sections == 0,
          "access: a two-factor requirement strips Manage Roles for an account without it");
    check(build(kSelf, 0), "access: owner deletion fixture");
    model.state.self_mfa = SelfMfaState::disabled;
    check(chat_state_store_set_guild_settings(&model.state, kGuild, settings) &&
              !guild_settings_can_delete(guild_settings_access(model.state, kGuild)),
          "access: an owner without 2FA cannot delete a server that requires it");
    check(build(kOwner, kPermissionAdministrator), "access: administrator fixture");
    access = guild_settings_access(model.state, kGuild);
    check(access.sections == 0x1ff && guild_settings_can_manage_guild(access) && !guild_settings_can_delete(access),
          "access: Administrator opens every section but not the owner actions");
}

// Guild-level permissions to the sections they open.
void test_section_matrix() noexcept {
    constexpr u32 p = guild_settings_profile | guild_settings_engagement;
    const struct {
        PermissionSet permissions;
        u32 sections;
    } rows[]{
        {0, 0},
        {kPermissionKickMembers, 0},
        {kPermissionModerateMembers | kPermissionKickMembers, 0},
        {kPermissionSendMessages | kPermissionManageChannels, 0},
        {kPermissionManageGuild, p | guild_settings_invites | guild_settings_safety | guild_settings_members},
        {kPermissionManageRoles, p | guild_settings_members | guild_settings_roles},
        {kPermissionBanMembers, p | guild_settings_members | guild_settings_bans},
        {kPermissionViewAuditLog, p | guild_settings_audit_log | guild_settings_bans},
        {kPermissionManageNicknames, p | guild_settings_members},
        {kPermissionManageNicknames | kPermissionKickMembers, p | guild_settings_members},
        {kPermissionCreateExpressions, p | guild_settings_expressions},
        {kPermissionManageExpressions, p | guild_settings_expressions},
        {kPermissionManageWebhooks, p},
        {kPermissionViewGuildInsights, p},
        {kPermissionAdministrator, 0x1ff},
        {kPermissionAll, 0x1ff},
    };
    u32 matched = 0;
    for (const auto& row : rows)
        matched += guild_settings_sections(row.permissions) == row.sections;
    check(matched == sizeof(rows) / sizeof(rows[0]),
          "section matrix: the opening permissions, Members with Manage Server, Bans with View Audit Log, and "
          "Kick or Timeout Members alone opening nothing");
}

void test_hierarchy() noexcept {
    check(build(kOwner, kPermissionManageRoles | kPermissionSendMessages), "hierarchy: fixture builds");
    const auto access = guild_settings_access(model.state, kGuild);
    const auto& s = model.state;
    const auto* top = chat_state_store_find_role(s, kGuild, kRoleTop);
    const auto* self = chat_state_store_find_role(s, kGuild, kRoleSelf);
    const auto* mid = chat_state_store_find_role(s, kGuild, kRoleMid);
    const auto* bot = chat_state_store_find_role(s, kGuild, kRoleBot);
    const auto* everyone = chat_state_store_find_role(s, kGuild, kGuild);
    check(!guild_settings_role_editable(access, *top) && !guild_settings_role_editable(access, *self) &&
              guild_settings_role_editable(access, *mid) && guild_settings_role_editable(access, *everyone),
          "hierarchy: roles at or above the highest role are read-only; lower roles and @everyone are editable");
    check(guild_settings_role_editable(access, *bot) && !guild_settings_role_deletable(access, *bot) &&
              guild_settings_role_deletable(access, *mid) && !guild_settings_role_deletable(access, *everyone),
          "hierarchy: managed roles and @everyone cannot be deleted");
    auto draft = guild_role_draft_from(s, *mid);
    check(guild_role_toggle(&draft, access, kPermissionSendMessages, true) &&
              !guild_role_toggle(&draft, access, kPermissionAdministrator, true) &&
              guild_role_toggle(&draft, access, kPermissionAttachFiles, false) &&
              draft.permissions == kPermissionSendMessages,
          "hierarchy: only held permissions can be granted; any permission can be removed");
}

void test_reorder() noexcept {
    check(build(kOwner, kPermissionManageRoles), "reorder: fixture builds");
    const auto access = guild_settings_access(model.state, kGuild);
    const RoleRecord* order[8];
    const u32 count = guild_settings_sorted_roles(model.state, kGuild, {order, 8});
    check(count == 6 && order[0]->id == kRoleTop && order[1]->id == kRoleSelf && order[2]->id == kRoleMid &&
              order[3]->id == kRoleBot && order[4]->id == kRoleLow && order[5]->id == kGuild,
          "reorder: roles sort by hierarchy with @everyone last");
    check(!guild_settings_role_move(access, {order, count}, 3, 1) &&
              !guild_settings_role_move(access, {order, count}, 0, 3) &&
              !guild_settings_role_move(access, {order, count}, 2, 5) && order[2]->id == kRoleMid,
          "reorder: rows cannot move above the highest role, roles above cannot move, @everyone stays last");
    check(guild_settings_role_move(access, {order, count}, 4, 2) && order[2]->id == kRoleLow &&
              order[3]->id == kRoleMid && order[4]->id == kRoleBot,
          "reorder: a lower role moves up within the editable range");
    GuildAdminPosition positions[8];
    const u32 written = guild_settings_role_positions({order, count}, kGuild, {positions, 8});
    check(written == 3 && positions[0].id == kRoleLow && positions[0].position == 4 && positions[1].id == kRoleMid &&
              positions[1].position == 3 && positions[2].id == kRoleBot && positions[2].position == 2 &&
              guild_settings_role_positions({order, count}, kGuild, {positions, 2}) == ~0u,
          "reorder: positions reuse existing values and include only moved roles");
    GuildAdminRequest request{};
    request.operation = GuildAdminOperation::role_reorder;
    request.guild_id = kGuild;
    request.positions = {positions, written};
    check(body_equals(request, R"([{"id":"13","position":4},{"id":"12","position":3},{"id":"14","position":2}])"),
          "reorder: the request body lists id and position pairs");
}

void test_drafts() noexcept {
    check(build(kSelf, 0), "drafts: owner fixture");
    auto base = guild_server_profile_from_state(model.state, kGuild);
    auto draft = base;
    GuildSettingsPatch patch{};
    check(!guild_server_profile_patch(base, draft, &patch), "drafts: an unchanged server name has nothing to save");
    copy_bytes(draft.name, "Renamed", 7);
    draft.name_bytes = 7;
    check(guild_server_profile_patch(base, draft, &patch) && patch.fields == guild_settings_patch_name,
          "drafts: a changed server name is its only field");
    GuildAdminRequest request{};
    request.operation = GuildAdminOperation::guild_update;
    request.guild_id = kGuild;
    request.guild = &patch;
    check(body_equals(request, R"({"name":"Renamed"})"), "drafts: the server name request body");
    draft.name_bytes = 1;
    check(!guild_server_profile_patch(base, draft, &patch), "drafts: one-character server names are refused");
    const auto* mid = chat_state_store_find_role(model.state, kGuild, kRoleMid);
    auto role_base = guild_role_draft_from(model.state, *mid), role_draft = role_base;
    u32 color = 0;
    check(guild_role_color_parse(utf8_literal("#3498db"), &color) && color == 0x3498db &&
              !guild_role_color_parse(utf8_literal("#3498d"), &color) &&
              !guild_role_color_parse(utf8_literal("zz98db"), &color),
          "drafts: hex colours parse with or without '#' and reject malformed input");
    char hex[8];
    check(guild_role_color_format(0x0a0b0c, {hex, 8}) == 7 && bytes_equal(hex, "#0A0B0C", 7),
          "drafts: colours format as #RRGGBB");
    role_draft.color = color;
    role_draft.hoist = true;
    RolePatch role_patch{};
    check(guild_role_patch(role_base, role_draft, &role_patch) &&
              role_patch.fields == (role_patch_color | role_patch_hoist),
          "drafts: role patches carry only changed fields");
    request = {};
    request.operation = GuildAdminOperation::role_update;
    request.guild_id = kGuild;
    request.target_id = kRoleMid;
    request.role = &role_patch;
    check(body_equals(request, R"({"color":3447003,"hoist":true})"), "drafts: role update body");
}

void test_overrides() noexcept {
    PermissionSet allow = 0, deny = 0;
    permission_tri_set(&allow, &deny, kPermissionSendMessages, PermissionTriState::deny);
    permission_tri_set(&allow, &deny, kPermissionAttachFiles, PermissionTriState::allow);
    check(deny == kPermissionSendMessages && allow == kPermissionAttachFiles &&
              permission_tri_get(allow, deny, kPermissionSendMessages) == PermissionTriState::deny &&
              permission_tri_get(allow, deny, kPermissionEmbedLinks) == PermissionTriState::neutral,
          "overrides: tri-state values map to allow and deny bits");
    permission_tri_set(&allow, &deny, kPermissionSendMessages, PermissionTriState::allow);
    permission_tri_set(&allow, &deny, kPermissionAttachFiles, PermissionTriState::neutral);
    check(allow == kPermissionSendMessages && deny == 0 &&
              permission_tri_get(kPermissionSendMessages, kPermissionSendMessages, kPermissionSendMessages) ==
                  PermissionTriState::allow,
          "overrides: switching state clears the opposite bit; malformed pairs read as allow");
    check(build(kOwner, kPermissionManageRoles), "overrides: fixture builds");
    const PermissionOverwriteInput everyone{kGuild, 0, kPermissionSendMessages, 0};
    check(chat_state_store_append_channel_overwrite(&model.state, kChannel, everyone),
          "overrides: stored override");
    const auto* channel = chat_state_store_find_channel(model.state, kChannel);
    const auto stored = chat_state_store_channel_overwrites(model.state, *channel);
    static PermissionOverwriteDraft draft;
    check(permission_overwrite_draft_load(&draft, stored) && permission_overwrite_draft_equal(draft, stored) &&
              permission_overwrite_draft_add(&draft, kRoleMid, 0) && !permission_overwrite_draft_add(&draft, kRoleMid, 0) &&
              !permission_overwrite_draft_equal(draft, stored) &&
              permission_overwrite_draft_set(&draft, kRoleMid, kPermissionAttachFiles, PermissionTriState::allow) &&
              permission_overwrite_draft_set(&draft, kGuild, kPermissionSendMessages, PermissionTriState::neutral),
          "overrides: drafts add targets once and edit their bits");
    PermissionOverwriteWrite writes[4];
    ChannelPatch patch{};
    patch.fields = channel_patch_overwrites;
    patch.overwrites = {writes, permission_overwrite_draft_write(draft, {writes, 4})};
    GuildAdminRequest request{};
    request.operation = GuildAdminOperation::channel_update;
    request.channel_id = kChannel;
    request.channel = &patch;
    check(patch.overwrites.count == 2 &&
              body_equals(request, R"({"permission_overwrites":[{"id":"100","type":0,"allow":"0","deny":"0"},)"
                                   R"({"id":"12","type":0,"allow":"32768","deny":"0"}]})"),
          "overrides: the channel PATCH replaces the full override list");
    check(permission_overwrite_draft_remove(&draft, kRoleMid) && permission_overwrite_draft_find(draft, kRoleMid) == ~0u &&
              permission_overwrite_draft_write(draft, {writes, 0}) == ~0u,
          "overrides: removal and short output are handled");
    u32 voice = 0, text = 0, guild_only = 0;
    const auto entries = permission_grid_entries();
    for (u32 i = 0; i < entries.count; ++i) {
        const auto& entry = entries[i];
        voice += permission_grid_applies(entry, permission_grid_channel_scope(2));
        text += permission_grid_applies(entry, permission_grid_channel_scope(0));
        guild_only += entry.scopes == permission_scope_guild;
    }
    const auto& view = permission_grid_entries()[0];
    check(voice && text && guild_only >= 10 && permission_grid_channel_scope(4) == permission_scope_channels &&
              utf8_equal(permission_grid_label(view, permission_scope_text), utf8_literal("View channel")) &&
              utf8_equal(permission_grid_label(view, permission_scope_guild), utf8_literal("View channels")),
          "overrides: the grid filters by channel type and renames channel-level entries");
}

void test_channel_and_bans() noexcept {
    ChannelSettingsDraft voice{};
    check(channel_settings_decode(
              utf8_literal(R"({"id":"9","guild_id":"100","type":2,"name":"Lounge","parent_id":null,"bitrate":64000,)"
                           R"("user_limit":5,"rate_limit_per_user":0,"nsfw":false,"permission_overwrites":[]})"),
              &voice) &&
              voice.bitrate == 64'000 && voice.user_limit == 5 && voice.type == 2 && voice.name_bytes == 6,
          "channel: voice details decode");
    ChannelSettingsDraft rejected = voice;
    check(!channel_settings_decode(utf8_literal(R"({"id":"9","guild_id":"100","type":0,"name":"text"})"), &rejected) &&
              rejected.bitrate == 64'000,
          "channel: text channels stay with the overview controller");
    auto draft = voice;
    draft.bitrate = 96'000;
    draft.user_limit = 0;
    ChannelPatch patch{};
    GuildAdminRequest request{};
    request.operation = GuildAdminOperation::channel_update;
    request.channel_id = 9;
    request.channel = &patch;
    check(channel_settings_patch(voice, draft, &patch) && body_equals(request, R"({"bitrate":96000,"user_limit":0})"),
          "channel: voice patch carries bitrate and limit");
    draft.user_limit = 100;
    check(!channel_settings_patch(voice, draft, &patch), "channel: user limits above 99 are refused");

    static GuildBanRecord records[4];
    GuildBanStore store{};
    GuildAdminBan page[2]{};
    page[0].user.id = 20;
    page[1].user.id = 30;
    check(guild_ban_store_bind(&store, {records, 4}) && (guild_ban_store_reset(&store, kGuild), true) &&
              guild_ban_store_append_page(&store, kGuild, {page, 2}, true) && store.complete &&
              guild_ban_store_apply_add(&store, kGuild, 25, utf8_literal("late")) &&
              guild_ban_store_apply_remove(&store, kGuild, 20) && store.count == 2 && store.records[0].user_id == 25 &&
              !guild_ban_store_apply_add(&store, 999, 40, {}),
          "bans: gateway additions and removals update the cached list in order");
}

void test_expressions_and_safety() noexcept {
    check(guild_emoji_slots(0, 0) == 50 && guild_emoji_slots(1, 0) == 100 && guild_emoji_slots(2, 0) == 150 &&
              guild_emoji_slots(3, 0) == 250 && guild_emoji_slots(0, guild_feature_more_emoji) == 200 &&
              guild_emoji_slots(3, guild_feature_more_emoji) == 250 && guild_emoji_slots(9, 0) == 250,
          "expressions: emoji slots follow the boost tier and MORE_EMOJI");
    check(guild_sticker_slots(0, 0) == 5 && guild_sticker_slots(1, 0) == 15 && guild_sticker_slots(2, 0) == 30 &&
              guild_sticker_slots(3, 0) == 60 && guild_sticker_slots(3, guild_feature_more_stickers) == 120 &&
              guild_sticker_slots(2, guild_feature_more_stickers) == 30,
          "expressions: sticker slots follow the boost tier; MORE_STICKERS applies at tier 3");
    check(!guild_banner_available(1, 0) && guild_banner_available(2, 0) && guild_banner_available(0, guild_feature_banner) &&
              !guild_role_icons_available(1, 0) && guild_role_icons_available(0, guild_feature_role_icons) &&
              !guild_animated_icon_available(0, 0) && guild_animated_icon_available(1, 0),
          "expressions: banner, role icons and animated icons unlock by tier or feature");
    char name[kGuildEmojiNameMaximum];
    const auto suggest = [&](Utf8View file, Utf8View expected) noexcept {
        const u32 bytes = guild_emoji_name_from_filename(file, {name, sizeof(name)});
        return utf8_equal({name, bytes}, expected) && guild_emoji_name_valid({name, bytes});
    };
    check(suggest(utf8_literal("C:\\pics\\party parrot.v2.gif"), utf8_literal("party_parrot_v2")) &&
              suggest(utf8_literal("x.png"), utf8_literal("x_")) &&
              suggest(utf8_literal("\xC3\xA9t\xC3\xA9.png"), utf8_literal("_t_")) &&
              suggest(utf8_literal("abcdefghijklmnopqrstuvwxyz0123456789.png"),
                      utf8_literal("abcdefghijklmnopqrstuvwxyz012345")),
          "expressions: emoji names come from file names with invalid characters replaced and bounds applied");
    check(build(kSelf, 0), "safety: fixture builds");
    GuildSettings settings{};
    settings.known = guild_settings_mfa_level | guild_settings_verification_level | guild_settings_explicit_content_filter;
    settings.mfa_level = 1;
    settings.verification_level = 2;
    settings.explicit_content_filter = 1;
    check(chat_state_store_set_guild_settings(&model.state, kGuild, settings), "safety: settings apply");
    const auto base = guild_safety_from_state(model.state, kGuild);
    auto draft = base;
    GuildSettingsPatch patch{};
    check(base.verification_level == 2 && base.explicit_content_filter == 1 && base.mfa_level == 1 &&
              !guild_safety_patch(base, draft, &patch),
          "safety: the draft mirrors the stored levels and an unchanged draft has no patch");
    draft.verification_level = 4;
    draft.explicit_content_filter = 2;
    GuildAdminRequest request{};
    request.operation = GuildAdminOperation::guild_update;
    request.guild_id = kGuild;
    request.guild = &patch;
    check(guild_safety_patch(base, draft, &patch) && !guild_safety_equal(base, draft) &&
              body_equals(request, R"({"verification_level":4,"explicit_content_filter":2})"),
          "safety: verification and filter changes serialize as one guild PATCH");
    draft = base;
    draft.mfa_level = 0;
    check(!guild_safety_patch(base, draft, &patch) && !guild_safety_equal(base, draft),
          "safety: a 2FA-only change is not part of the guild PATCH");
    draft.verification_level = 5;
    check(!guild_safety_patch(base, draft, &patch), "safety: out-of-range levels are refused");
    check(guild_delete_confirmed(utf8_literal("  My Server "), utf8_literal("My Server")) &&
              !guild_delete_confirmed(utf8_literal("my server"), utf8_literal("My Server")) &&
              !guild_delete_confirmed(utf8_literal("My Serve"), utf8_literal("My Server")) &&
              !guild_delete_confirmed({}, {}),
          "delete: the confirmation needs the exact server name");
}

// Synthetic details: flags 0x880 (two bits the page never shows) and four
// names, one of them unknown, in server order.
struct Details {
    GuildDetailStore store;
    GuildDetailRecord records[2];
    char text[1024];
};
Details details;
constexpr u8 kNames[] = "\x04" "NEWS" "\x09" "COMMUNITY" "\x1E" "ACTIVITY_FEED_DISABLED_BY_USER" "\x0E"
                        "FUTURE_FEATURE";

bool engagement_fixture(u64 owner, PermissionSet permissions, u32 member_count) noexcept {
    if (!build(owner, permissions))
        return false;
    clear_bytes(&details, sizeof(details));
    if (!guild_detail_store_bind(&details.store, {details.records, 2}, {details.text, sizeof(details.text)}))
        return false;
    GuildDetailInput input{};
    input.guild_id = kGuild;
    input.known = static_cast<u16>(guild_detail_all & ~guild_detail_member_count);
    if (member_count) {
        input.known = guild_detail_all;
        input.member_count = member_count;
    }
    input.system_channel_flags = 0x880;
    input.feature_names = {kNames, sizeof(kNames) - 1};
    input.feature_count = 4;
    input.keep_feature_names = true;
    GuildSettings settings{};
    settings.known = guild_settings_system_channel | guild_settings_afk_timeout;
    settings.system_channel_id = kChannel;
    settings.afk_timeout_seconds = 900;
    return guild_detail_store_apply(&details.store, input) &&
           chat_state_store_set_guild_settings(&model.state, kGuild, settings);
}

bool engagement_body(const GuildEngagementDraft& base, const GuildEngagementDraft& draft, const char* expected) noexcept {
    const auto* record = guild_detail_store_find(details.store, kGuild);
    u8 names[512];
    GuildSettingsPatch patch{};
    if (!record || !guild_engagement_patch(base, draft, guild_detail_store_feature_names(details.store, *record),
                                           record->feature_count, {names, sizeof(names)}, &patch))
        return false;
    GuildAdminRequest request{};
    request.operation = GuildAdminOperation::guild_update;
    request.guild_id = kGuild;
    request.guild = &patch;
    return body_equals(request, expected);
}

void test_engagement() noexcept {
    check(engagement_fixture(kOwner, kPermissionManageGuild, 9'999), "engagement: fixture builds");
    const auto base = guild_engagement_from_state(model.state, details.store, kGuild);
    auto draft = base;
    check(base.system_channel_id == kChannel && base.afk_timeout_seconds == 900 && base.system_channel_flags == 0x880 &&
              (base.features & guild_feature_activity_feed_disabled) && guild_engagement_equal(base, draft) &&
              guild_engagement_next_step(base, draft, false) == GuildEngagementStep::none,
          "engagement: the draft mirrors the gateway settings and details; nothing to save");
    check(guild_engagement_message_enabled(draft, guild_system_suppress_join), "engagement: a clear bit reads as send");
    guild_engagement_set_message(&draft, guild_system_suppress_join, false);
    check(draft.system_channel_flags == 0x881 && !guild_engagement_message_enabled(draft, guild_system_suppress_join),
          "engagement: turning one message off sets only its suppress bit (0x880 to 0x881)");
    check(engagement_body(base, draft,
                          R"({"afk_channel_id":null,"afk_timeout":900,"system_channel_id":"500",)"
                          R"("default_message_notifications":0,"system_channel_flags":2177})"),
          "engagement: the PATCH sends all five scalars in order, without features when the set is unchanged");
    guild_engagement_set_message(&draft, guild_system_suppress_join, true);
    check(guild_engagement_equal(base, draft) && draft.system_channel_flags == 0x880,
          "engagement: turning it back on restores the stored flags");

    check(!guild_engagement_activity_feed(draft, 9'999, true), "engagement: the disabled feature wins over the size");
    guild_engagement_set_activity_feed(&draft, true);
    check(guild_engagement_activity_feed(draft, 50'000, true) && guild_engagement_features_changed(base, draft) &&
              engagement_body(base, draft,
                              R"({"features":["NEWS","COMMUNITY","FUTURE_FEATURE","ACTIVITY_FEED_ENABLED_BY_USER"],)"
                              R"("afk_channel_id":null,"afk_timeout":900,"system_channel_id":"500",)"
                              R"("default_message_notifications":0,"system_channel_flags":2176})"),
          "engagement: enabling the feed sends the full list in server order, the new name appended and the "
          "disabled one removed");
    guild_engagement_set_activity_feed(&draft, false);
    check(!guild_engagement_features_changed(base, draft) && guild_engagement_equal(base, draft),
          "engagement: switching the feed back leaves the same set, so features are not sent");
    GuildEngagementDraft fresh{};
    check(guild_engagement_activity_feed(fresh, 9'999, true) && !guild_engagement_activity_feed(fresh, 10'000, true) &&
              !guild_engagement_activity_feed(fresh, 0, false),
          "engagement: without either feature the feed is on below 10,000 known members");
    draft = base;
    guild_engagement_set_activity_feed(&draft, true);
    u8 names[512];
    GuildSettingsPatch patch{};
    check(!guild_engagement_patch(base, draft, {}, 0, {names, sizeof(names)}, &patch),
          "engagement: a features change without the stored names cannot be saved");

    // Footer: a widget-only change still sends the guild PATCH first, then the widget.
    auto loaded = base, widget = base;
    guild_engagement_set_widget(&loaded, false, 0);
    widget = loaded;
    guild_engagement_set_widget(&widget, true, kChannel);
    check(!guild_engagement_equal(loaded, widget) &&
              guild_engagement_next_step(loaded, widget, false) == GuildEngagementStep::guild &&
              guild_engagement_next_step(loaded, widget, true) == GuildEngagementStep::widget,
          "engagement: the Save bar sends the guild PATCH for any change, then the widget PATCH");
    guild_engagement_widget_patch(widget, &patch);
    GuildAdminRequest request{};
    request.operation = GuildAdminOperation::widget_update;
    request.guild_id = kGuild;
    request.guild = &patch;
    check(body_equals(request, R"({"enabled":true,"channel_id":"500"})"), "engagement: the widget PATCH body");
    auto saved = widget;
    check(guild_engagement_next_step(saved, widget, true) == GuildEngagementStep::none &&
              guild_engagement_next_step(saved, widget, false) == GuildEngagementStep::none &&
              guild_engagement_next_step(base, base, true) == GuildEngagementStep::none,
          "engagement: after both PATCHes the page is clean; with no widget change the guild PATCH ends the save");

    const auto manager = guild_settings_access(model.state, kGuild);
    check(engagement_fixture(kOwner, kPermissionBanMembers, 0), "engagement: moderator fixture");
    const auto moderator = guild_settings_access(model.state, kGuild);
    check(guild_settings_can_manage_guild(manager) && (moderator.sections & guild_settings_engagement) &&
              !guild_settings_can_manage_guild(moderator),
          "engagement: visible to anyone who opens Server Settings, editable only with Manage Server");
}

} // namespace guild_settings_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_guild_settings_entry() noexcept {
    using namespace maicord::guild_settings_test;
    test_access();
    test_section_matrix();
    test_engagement();
    test_hierarchy();
    test_reorder();
    test_drafts();
    test_overrides();
    test_channel_and_bans();
    test_expressions_and_safety();
    constexpr char pass[] = "PASS guild settings model\n";
    constexpr char fail[] = "FAIL guild settings model\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}
