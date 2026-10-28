#include "maicord/model/guild_settings_model.hpp"
#include "maicord/model/channel_reorder.hpp"
#include "maicord/protocol/guild_assets.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/color_hex.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/permission_compute.hpp"
#include "maicord/model/guild_detail_store.hpp"

namespace maicord {
namespace {
bool rank_above(u32 position, u64 id, const RoleRecord& role) noexcept {
    return position > role.position || (position == role.position && id < role.id);
}
bool name_valid(Utf8View text, u32 minimum_codepoints) noexcept {
    const auto validation = utf8_validate(text);
    if (!validation.valid || validation.codepoints < minimum_codepoints ||
        validation.codepoints > kGuildAdminNameCodepoints)
        return false;
    bool visible = false;
    for (u32 i = 0; i < text.bytes; ++i) {
        const u8 byte = static_cast<u8>(text.data[i]);
        if (byte < 0x20)
            return false;
        visible = visible || byte != ' ';
    }
    return visible;
}
void copy_name(Utf8View source, char* destination, u16* bytes) noexcept {
    const u32 count = minimum(source.bytes, kGuildAdminNameBytes);
    copy_bytes(destination, source.data, count);
    *bytes = static_cast<u16>(count);
}
bool same_bytes(const char* a, u32 a_bytes, const char* b, u32 b_bytes) noexcept {
    return a_bytes == b_bytes && bytes_equal(a, b, a_bytes);
}
// IDs of the roles the account holds in a guild, @everyone excluded.
u32 self_role_ids(const ChatStateStore& store, u64 guild_id, u64 (&roles)[kGuildRoleMaximum]) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < store.role_count && count < kGuildRoleMaximum; ++i)
        if (store.roles[i].guild_id == guild_id && store.roles[i].id != guild_id && store.roles[i].self_assigned)
            roles[count++] = store.roles[i].id;
    return count;
}
} // namespace

const u32 kGuildRoleColors[kGuildRoleColorCount]{
    0x1abc9c, 0x2ecc71, 0x3498db, 0x9b59b6, 0xe91e63, 0xf1c40f, 0xe67e22, 0xe74c3c, 0x95a5a6, 0x607d8b,
    0x11806a, 0x1f8b4c, 0x206694, 0x71368a, 0xad1457, 0xc27c0e, 0xa84300, 0x992d22, 0x979c9f, 0x546e7a};

GuildSettingsAccess guild_settings_access(const ChatStateStore& store, u64 guild_id) noexcept {
    GuildSettingsAccess access{};
    const auto* guild = chat_state_store_find_guild(store, guild_id);
    if (!guild || guild->unavailable || !store.self_user_id || !guild->owner_id)
        return access;
    access.guild_id = guild_id;
    access.self_user_id = store.self_user_id;
    access.owner_id = guild->owner_id;
    access.owner = guild->owner_id == store.self_user_id;
    access.resolved = guild->permissions_resolved && (access.owner || guild->member_roles_known);
    if (!access.resolved)
        return access;
    u64 roles[kGuildRoleMaximum];
    const u32 count = self_role_ids(store, guild_id, roles);
    const PermissionMember self{store.self_user_id, {roles, count}, 0,
                                guild_self_quarantined(guild->settings)};
    access.permissions = permission_compute(chat_state_store_permission_guild(store, guild_id), self, {},
                                            {0, store.self_mfa == SelfMfaState::disabled});
    if (const auto* top = chat_state_store_highest_role(store, guild_id, {roles, count})) {
        access.top_role_id = top->id;
        access.top_position = top->position;
    }
    access.sections = guild_settings_sections(access.permissions);
    access.manage_expressions = permission_has(access.permissions, kPermissionManageExpressions);
    const bool server_mfa = (guild->settings.known & guild_settings_mfa_level) && guild->settings.mfa_level;
    access.delete_allowed = access.owner && (store.self_mfa == SelfMfaState::enabled || !server_mfa);
    return access;
}

u32 guild_settings_sections(PermissionSet p) noexcept {
    if (!(p & kGuildSettingsOpenPermissions))
        return 0;
    u32 sections = guild_settings_profile | guild_settings_engagement | guild_settings_boost_perks;
    if (permission_has(p, kPermissionManageGuild))
        sections |= guild_settings_invites | guild_settings_safety;
    if (permission_has(p, kPermissionManageExpressions) || permission_has(p, kPermissionCreateExpressions))
        sections |= guild_settings_expressions;
    if (p & kGuildSettingsMemberPermissions)
        sections |= guild_settings_members;
    if (permission_has(p, kPermissionManageRoles))
        sections |= guild_settings_roles;
    if (permission_has(p, kPermissionViewAuditLog))
        sections |= guild_settings_audit_log;
    if (permission_has(p, kPermissionBanMembers) || permission_has(p, kPermissionViewAuditLog))
        sections |= guild_settings_bans;
    return sections;
}

bool guild_settings_can_manage_guild(const GuildSettingsAccess& access) noexcept {
    return access.guild_id && access.sections && permission_has(access.permissions, kPermissionManageGuild);
}

bool guild_settings_can_unban(const GuildSettingsAccess& access) noexcept {
    return access.guild_id && access.sections && permission_has(access.permissions, kPermissionBanMembers);
}

bool guild_settings_can_delete(const GuildSettingsAccess& access) noexcept {
    return access.guild_id && access.delete_allowed;
}

bool guild_settings_can_set_mfa(const GuildSettingsAccess& access) noexcept {
    return access.guild_id && access.owner;
}

u32 guild_emoji_slots(u8 premium_tier, u64 features) noexcept {
    static constexpr u32 slots[]{50, 100, 150, 250};
    const u32 tier = slots[premium_tier < 4 ? premium_tier : 3];
    return (features & guild_feature_more_emoji) ? maximum(tier, 200u) : tier;
}

u32 guild_sticker_slots(u8 premium_tier, u64 features) noexcept {
    static constexpr u32 slots[]{5, 15, 30, 60};
    const u8 tier = premium_tier < 4 ? premium_tier : 3;
    return (features & guild_feature_more_stickers) && tier == 3 ? 120u : slots[tier];
}

bool guild_role_icons_available(u8 premium_tier, u64 features) noexcept {
    return premium_tier >= 2 || (features & guild_feature_role_icons);
}

GuildEmojiCounts guild_emoji_counts(const ChatStateStore& store, u64 guild_id) noexcept {
    GuildEmojiCounts counts{};
    for (u32 i = 0; i < store.emoji_count; ++i) {
        const auto& emoji = store.emojis[i];
        if (emoji.guild_id != guild_id)
            continue;
        if (emoji.flags & custom_emoji_animated)
            ++counts.animated_count;
        else
            ++counts.static_count;
    }
    return counts;
}

u32 guild_emoji_name_from_filename(Utf8View filename, Slice<char> output) noexcept {
    if (!output.data || output.count < kGuildEmojiNameMaximum)
        return 0;
    u32 start = 0, end = filename.bytes;
    for (u32 i = 0; i < filename.bytes; ++i)
        if (filename.data[i] == '\\' || filename.data[i] == '/')
            start = i + 1;
    for (u32 i = end; i > start; --i)
        if (filename.data[i - 1] == '.') {
            end = i - 1;
            break;
        }
    u32 bytes = 0;
    for (u32 i = start; i < end && bytes < kGuildEmojiNameMaximum; ++i) {
        const char c = filename.data[i];
        const bool keep = ascii_is_alnum(c) || c == '_';
        // A multi-byte character becomes one underscore.
        if (!keep && (static_cast<u8>(c) & 0xC0) == 0x80)
            continue;
        output[bytes++] = keep ? c : '_';
    }
    while (bytes < kGuildEmojiNameMinimum)
        output[bytes++] = '_';
    return bytes;
}

GuildSafetyDraft guild_safety_from_state(const ChatStateStore& store, u64 guild_id) noexcept {
    GuildSafetyDraft draft{};
    if (const auto* guild = chat_state_store_find_guild(store, guild_id)) {
        draft.verification_level = guild->settings.verification_level;
        draft.explicit_content_filter = guild->settings.explicit_content_filter;
        draft.mfa_level = guild->settings.mfa_level;
    }
    return draft;
}

bool guild_safety_equal(const GuildSafetyDraft& a, const GuildSafetyDraft& b) noexcept {
    return a.verification_level == b.verification_level && a.explicit_content_filter == b.explicit_content_filter &&
           a.mfa_level == b.mfa_level;
}

bool guild_safety_patch(const GuildSafetyDraft& baseline, const GuildSafetyDraft& draft,
                        GuildSettingsPatch* patch) noexcept {
    if (!patch || draft.verification_level > 4 || draft.explicit_content_filter > 2)
        return false;
    GuildSettingsPatch value{};
    if (baseline.verification_level != draft.verification_level) {
        value.fields |= guild_settings_patch_verification_level;
        value.verification_level = draft.verification_level;
    }
    if (baseline.explicit_content_filter != draft.explicit_content_filter) {
        value.fields |= guild_settings_patch_explicit_content_filter;
        value.explicit_content_filter = draft.explicit_content_filter;
    }
    if (!value.fields)
        return false;
    *patch = value;
    return true;
}

bool guild_delete_confirmed(Utf8View typed, Utf8View name) noexcept {
    u32 start = 0, end = typed.bytes;
    while (start < end && typed.data[start] == ' ')
        ++start;
    while (end > start && typed.data[end - 1] == ' ')
        --end;
    return name.bytes && end - start == name.bytes && utf8_equal({typed.data + start, end - start}, name);
}

// Strictly below the account's highest role; the owner outranks every role.
static bool guild_settings_role_below(const GuildSettingsAccess& access, const RoleRecord& role) noexcept {
    if (!access.guild_id || role.guild_id != access.guild_id)
        return false;
    return access.owner || (access.top_role_id && rank_above(access.top_position, access.top_role_id, role));
}

bool guild_settings_role_editable(const GuildSettingsAccess& access, const RoleRecord& role) noexcept {
    return (access.sections & guild_settings_roles) && guild_settings_role_below(access, role);
}

GuildRoleLock guild_settings_role_lock(const GuildSettingsAccess& access, const RoleRecord& role) noexcept {
    if (!(access.sections & guild_settings_roles))
        return GuildRoleLock::no_permission;
    if (guild_settings_role_below(access, role))
        return GuildRoleLock::none;
    return role.id == access.top_role_id ? GuildRoleLock::highest : GuildRoleLock::above;
}

bool guild_settings_can_view_as_role(const GuildSettingsAccess& access, const RoleRecord& role) noexcept {
    if (!access.resolved || role.guild_id != access.guild_id)
        return false;
    if (access.owner)
        return true;
    return permission_has(access.permissions, kPermissionManageGuild) &&
           permission_has(access.permissions, kPermissionManageRoles) &&
           (role.id == access.guild_id || role.id == access.top_role_id || guild_settings_role_below(access, role));
}

static PermissionSet guild_settings_self_permissions_with(const ChatStateStore& store, const GuildSettingsAccess& access,
                                                          u64 role_id, PermissionSet role_permissions) noexcept {
    const auto* guild = chat_state_store_find_guild(store, access.guild_id);
    if (!guild || !access.resolved)
        return 0;
    u64 roles[kGuildRoleMaximum];
    const u32 count = self_role_ids(store, access.guild_id, roles);
    const auto permission_guild = chat_state_store_permission_guild(store, access.guild_id);
    const PermissionMember self{store.self_user_id, {roles, count}, 0, guild_self_quarantined(guild->settings)};
    return permission_compute_from_base(permission_compute_base_with(permission_guild, self, role_id, role_permissions),
                                        permission_guild, self, {}, {0, store.self_mfa == SelfMfaState::disabled});
}

bool guild_settings_role_deletable(const GuildSettingsAccess& access, const RoleRecord& role) noexcept {
    return guild_settings_role_editable(access, role) && !role.managed && role.id != access.guild_id;
}

PermissionSet guild_settings_grantable(const GuildSettingsAccess& access) noexcept {
    return access.owner || (access.permissions & kPermissionAdministrator) ? kPermissionKnownMask
                                                                         : access.permissions;
}

u32 guild_settings_sorted_roles(const ChatStateStore& store, u64 guild_id,
                                Slice<const RoleRecord*> output) noexcept {
    if (!output.data || !output.count)
        return 0;
    u32 count = 0;
    const RoleRecord* everyone = nullptr;
    for (u32 i = 0; i < store.role_count; ++i) {
        const auto& role = store.roles[i];
        if (role.guild_id != guild_id)
            continue;
        if (role.id == guild_id)
            everyone = &role;
        else if (count < output.count)
            output[count++] = &role;
    }
    heap_sort(output.data, count, [](const RoleRecord* a, const RoleRecord* b) noexcept {
        return rank_above(a->position, a->id, *b);
    });
    if (everyone && count < output.count)
        output[count++] = everyone;
    return count;
}

bool guild_settings_role_move(const GuildSettingsAccess& access, Slice<const RoleRecord*> order, u32 from,
                              u32 to) noexcept {
    u32 movable = order.count;
    if (movable && order[movable - 1] && order[movable - 1]->id == access.guild_id)
        --movable;
    if (!(access.sections & guild_settings_roles) || from >= movable || to >= movable || from == to ||
        !order[from])
        return false;
    // Rows at or above the account's highest role form a fixed prefix.
    u32 first = 0;
    while (first < movable && !guild_settings_role_below(access, *order[first]))
        ++first;
    if (from < first || to < first)
        return false;
    const auto* moving = order[from];
    if (from < to)
        for (u32 i = from; i < to; ++i)
            order[i] = order[i + 1];
    else
        for (u32 i = from; i > to; --i)
            order[i] = order[i - 1];
    order[to] = moving;
    return true;
}

u32 guild_settings_role_positions(ConstSlice<const RoleRecord*> order, u64 guild_id,
                                  Slice<GuildAdminPosition> output) noexcept {
    // Reuse the existing position values in descending order, so only the
    // moved range changes and roles above the account keep theirs.
    u32 slots[kGuildRoleMaximum];
    u32 count = 0;
    for (u32 i = 0; i < order.count && count < kGuildRoleMaximum; ++i)
        if (order[i] && order[i]->id != guild_id)
            slots[count++] = order[i]->position;
    heap_sort(slots, count, [](u32 a, u32 b) noexcept { return a > b; });
    u32 used = 0, slot = 0;
    for (u32 i = 0; i < order.count && slot < count; ++i) {
        if (!order[i] || order[i]->id == guild_id)
            continue;
        const u32 position = slots[slot++];
        if (order[i]->position == position)
            continue;
        if (used >= output.count)
            return ~0u;
        output[used++] = {order[i]->id, 0, position, 0};
    }
    return used;
}

const Utf8View kGuildProfileBannerColors[kGuildProfileBannerColorCount]{
    utf8_literal("#ff1c90"), utf8_literal("#e81d1e"), utf8_literal("#e86e1d"),
    utf8_literal("#e8c02f"), utf8_literal("#71368a"), utf8_literal("#029FFC"),
    utf8_literal("#4fe2ca"), utf8_literal("#406601"), utf8_literal("#272727")};
const Utf8View kGuildProfileBannerNames[kGuildProfileBannerColorCount]{
    utf8_literal("Soul"),    utf8_literal("Volcano"), utf8_literal("Marsh"),
    utf8_literal("Thunder"), utf8_literal("Rising"),  utf8_literal("Zephyr"),
    utf8_literal("Cascade"), utf8_literal("Earth"),   utf8_literal("Boulder")};

namespace {
constexpr u8 kVisibilityPublic = 1, kVisibilityRestricted = 2, kVisibilityPublicWithRecruitment = 3;
} // namespace

GuildServerProfileDraft guild_server_profile_draft(const GuildAdminProfile& profile) noexcept {
    GuildServerProfileDraft draft{};
    draft.profile = profile;
    draft.icon = GuildProfileImageEdit::keep;
    return draft;
}

bool guild_server_profile_equal(const GuildServerProfileDraft& a, const GuildServerProfileDraft& b) noexcept {
    const auto& x = a.profile;
    const auto& y = b.profile;
    return a.icon == b.icon && x.name_known == y.name_known && same_bytes(x.name, x.name_bytes, y.name, y.name_bytes) &&
           same_bytes(x.description, x.description_bytes, y.description, y.description_bytes) &&
           x.visibility_known == y.visibility_known && x.visibility == y.visibility &&
           same_bytes(x.brand_color, x.brand_color_bytes, y.brand_color, y.brand_color_bytes) &&
           x.custom_banner_known == y.custom_banner_known &&
           same_bytes(x.custom_banner, x.custom_banner_bytes, y.custom_banner, y.custom_banner_bytes);
}

bool guild_server_name_valid(Utf8View text) noexcept {
    return name_valid(text, 2);
}

bool guild_server_description_valid(Utf8View text) noexcept {
    const auto validation = utf8_validate(text);
    return validation.valid && validation.codepoints <= kGuildAdminProfileDescriptionCodepoints &&
           text.bytes <= kGuildAdminProfileDescriptionBytes;
}

bool guild_server_profile_private(const GuildServerProfileDraft& draft) noexcept {
    const auto& p = draft.profile;
    return !p.visibility_known || (p.visibility != kVisibilityPublic && p.visibility != kVisibilityPublicWithRecruitment);
}

void guild_server_profile_set_private(GuildServerProfileDraft* draft, const GuildAdminProfile& stored, bool on) noexcept {
    if (!draft)
        return;
    draft->profile.visibility_known = true;
    draft->profile.visibility =
        on ? kVisibilityRestricted
           : (stored.visibility_known && stored.visibility == kVisibilityPublicWithRecruitment
                  ? kVisibilityPublicWithRecruitment
                  : kVisibilityPublic);
}

u32 guild_server_profile_banner_choice(const GuildServerProfileDraft& draft) noexcept {
    const auto& p = draft.profile;
    if (p.custom_banner_bytes)
        return ~0u;
    if (!p.brand_color_bytes)
        return 0;
    for (u32 i = 0; i < kGuildProfileBannerColorCount; ++i)
        if (ascii_equal_ignore_case({p.brand_color, p.brand_color_bytes}, kGuildProfileBannerColors[i]))
            return i + 1;
    return ~0u;
}

void guild_server_profile_set_banner_choice(GuildServerProfileDraft* draft, u32 choice) noexcept {
    if (!draft || choice > kGuildProfileBannerColorCount)
        return;
    auto& p = draft->profile;
    const Utf8View color = choice ? kGuildProfileBannerColors[choice - 1] : Utf8View{};
    copy_bytes(p.brand_color, color.data, color.bytes);
    p.brand_color_bytes = static_cast<u8>(color.bytes);
    p.custom_banner_bytes = 0;
    p.custom_banner_known = true;
}

bool guild_server_profile_write(const GuildServerProfileDraft& draft, Utf8View icon_uri, const ChatStateStore& state,
                                u64 guild_id, Slice<GuildServerProfileTraitWrite> traits,
                                GuildServerProfileWrite* write) noexcept {
    const auto& p = draft.profile;
    if (!write || !traits.data || traits.count < kGuildAdminProfileTraits ||
        (draft.icon == GuildProfileImageEdit::replace && !icon_uri.bytes) ||
        (p.name_known && !guild_server_name_valid({p.name, p.name_bytes})) ||
        !guild_server_description_valid({p.description, p.description_bytes}))
        return false;
    GuildServerProfileWrite value{};
    if (p.name_known) {
        value.fields |= guild_profile_write_name;
        value.name = {p.name, p.name_bytes};
    }
    value.description = {p.description, p.description_bytes};
    if (draft.icon != GuildProfileImageEdit::keep || p.icon_known) {
        value.fields |= guild_profile_write_icon;
        value.icon = draft.icon == GuildProfileImageEdit::replace  ? icon_uri
                     : draft.icon == GuildProfileImageEdit::remove ? Utf8View{}
                                                                   : Utf8View{p.icon, p.icon_bytes};
    }
    if (p.custom_banner_known) {
        value.fields |= guild_profile_write_custom_banner;
        value.custom_banner = {p.custom_banner, p.custom_banner_bytes};
    }
    if (p.visibility_known) {
        value.fields |= guild_profile_write_visibility;
        value.visibility = p.visibility;
    }
    value.brand_color_primary = {p.brand_color, p.brand_color_bytes};
    u32 count = 0;
    for (u32 i = 0; i < kGuildAdminProfileTraits; ++i) {
        const auto& t = p.traits[i];
        if (!t.label_bytes)
            continue;
        GuildServerProfileTraitWrite trait{};
        trait.label = {t.label, t.label_bytes};
        trait.position = static_cast<u8>(i);
        // A custom emoji is looked up among the guild's emoji and written from
        // that record; one that is gone is left out.
        if (t.emoji_id) {
            for (u32 e = 0; e < state.emoji_count; ++e) {
                const auto& emoji = state.emojis[e];
                if (emoji.id != t.emoji_id || emoji.guild_id != guild_id)
                    continue;
                trait.emoji_id = emoji.id;
                trait.emoji_name = {emoji.name, emoji.name_bytes};
                trait.emoji_animated = (emoji.flags & custom_emoji_animated) != 0;
                trait.emoji_animated_set = true;
                break;
            }
        } else if (t.emoji_name_bytes)
            trait.emoji_name = {t.emoji_name, t.emoji_name_bytes};
        traits[count++] = trait;
    }
    value.traits = {traits.data, count};
    value.game_application_ids = {p.game_application_ids, p.game_count};
    *write = value;
    return true;
}

bool guild_boost_equal(const GuildBoostDraft& a, const GuildBoostDraft& b) noexcept {
    return a.banner == b.banner && a.splash == b.splash && a.progress_bar == b.progress_bar;
}

bool guild_boost_patch(const GuildBoostDraft& draft, Utf8View banner_hash, Utf8View splash_hash, Utf8View banner_uri,
                       Utf8View splash_uri, GuildSettingsPatch* patch) noexcept {
    if (!patch || (draft.banner == GuildProfileImageEdit::replace && !banner_uri.bytes) ||
        (draft.splash == GuildProfileImageEdit::replace && !splash_uri.bytes))
        return false;
    const auto image = [](GuildProfileImageEdit edit, Utf8View hash, Utf8View uri) noexcept {
        return edit == GuildProfileImageEdit::replace ? uri : edit == GuildProfileImageEdit::remove ? Utf8View{} : hash;
    };
    GuildSettingsPatch value{};
    value.fields = guild_settings_patch_splash | guild_settings_patch_banner | guild_settings_patch_progress_bar;
    value.splash = image(draft.splash, splash_hash, splash_uri);
    value.banner = image(draft.banner, banner_hash, banner_uri);
    value.premium_progress_bar_enabled = draft.progress_bar;
    *patch = value;
    return true;
}

u32 guild_vanity_code_input(Utf8View typed, Slice<char> output) noexcept {
    if (!output.data)
        return 0;
    u32 bytes = 0, units = 0;
    for (u32 i = 0; i < typed.bytes;) {
        const u8 lead = static_cast<u8>(typed.data[i]);
        const u32 length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        const u32 width = length == 4 ? 2 : 1;
        if (i + length > typed.bytes || units + width > kGuildAdminVanityCodeCharacters ||
            bytes + length > output.count)
            break;
        for (u32 j = 0; j < length; ++j)
            output[bytes + j] = typed.data[i + j] == ' ' ? '-' : typed.data[i + j];
        bytes += length;
        units += width;
        i += length;
    }
    return bytes;
}

Utf8View guild_vanity_error_text(u64 code) noexcept {
    switch (code) {
    case 10'003:
        return utf8_literal("You need to set a text channel that is accessible to all members before setting a "
                            "Custom Invite Link.");
    case 50'013: return utf8_literal("You do not have access to this feature.");
    case 50'001: return utf8_literal("Your server does not have access to this feature.");
    case 20'040: return utf8_literal("You cannot clear your custom invite URL while your server web page is published.");
    case 20'044: return utf8_literal("Can't set vanity url for an internal employee only server");
    case 20'045:
        return utf8_literal("Custom Invite Link requires Server Boosting Level 3, or being a partnered, verified, "
                            "HypeSquad event server");
    default:
        return utf8_literal("Custom Invite Link could not be changed because it contains invalid characters, was too "
                            "short, or is already taken.\nCustom Invite Links can only contain letters, numbers, or "
                            "dashes.");
    }
}

bool guild_settings_channel_before(const ChatStateStore& state, GuildChannelPickerOrder order, const ChannelRecord& a,
                                   const ChannelRecord& b) noexcept {
    if (order == GuildChannelPickerOrder::channel_list)
        return channel_list_before(state, a, b);
    const u32 x = channel_list_position(a.type, a.position), y = channel_list_position(b.type, b.position);
    return x != y ? x < y : a.id < b.id;
}

// Engagement ----------------------------------------------------------------

GuildEngagementDraft guild_engagement_from_state(const ChatStateStore& store, const GuildDetailStore& details,
                                                 u64 guild_id) noexcept {
    GuildEngagementDraft draft{};
    const auto* guild = chat_state_store_find_guild(store, guild_id);
    if (!guild)
        return draft;
    draft.system_channel_id = guild->settings.system_channel_id;
    draft.afk_channel_id = guild->settings.afk_channel_id;
    draft.afk_timeout_seconds = guild->settings.afk_timeout_seconds ? guild->settings.afk_timeout_seconds : 300;
    draft.default_notifications = guild->settings.default_message_notifications ? 1 : 0;
    if (const auto* record = guild_detail_store_find(details, guild_id)) {
        draft.system_channel_flags = record->system_channel_flags;
        draft.features = record->features;
    }
    return draft;
}

void guild_engagement_set_widget(GuildEngagementDraft* draft, bool enabled, u64 channel_id) noexcept {
    if (draft) {
        draft->widget_enabled = enabled;
        draft->widget_channel_id = channel_id;
    }
}

bool guild_engagement_features_changed(const GuildEngagementDraft& a, const GuildEngagementDraft& b) noexcept {
    return a.features != b.features;
}

bool guild_engagement_widget_changed(const GuildEngagementDraft& a, const GuildEngagementDraft& b) noexcept {
    return a.widget_enabled != b.widget_enabled || a.widget_channel_id != b.widget_channel_id;
}

bool guild_engagement_equal(const GuildEngagementDraft& a, const GuildEngagementDraft& b) noexcept {
    return a.system_channel_id == b.system_channel_id && a.afk_channel_id == b.afk_channel_id &&
           a.afk_timeout_seconds == b.afk_timeout_seconds && a.system_channel_flags == b.system_channel_flags &&
           a.default_notifications == b.default_notifications && !guild_engagement_features_changed(a, b) &&
           !guild_engagement_widget_changed(a, b);
}

bool guild_engagement_message_enabled(const GuildEngagementDraft& draft, u32 suppress_bit) noexcept {
    return !(draft.system_channel_flags & suppress_bit);
}

void guild_engagement_set_message(GuildEngagementDraft* draft, u32 suppress_bit, bool send) noexcept {
    if (draft)
        draft->system_channel_flags = send ? draft->system_channel_flags & ~suppress_bit
                                           : draft->system_channel_flags | suppress_bit;
}

void guild_engagement_add_feature(GuildEngagementDraft* draft, u64 bit) noexcept {
    if (!draft || !bit || (draft->features & bit) || draft->appended_count >= 4)
        return;
    draft->features |= bit;
    draft->appended[draft->appended_count++] = bit;
}

void guild_engagement_remove_feature(GuildEngagementDraft* draft, u64 bit) noexcept {
    if (!draft || !bit || !(draft->features & bit))
        return;
    draft->features &= ~bit;
    u32 kept = 0;
    for (u32 i = 0; i < draft->appended_count; ++i)
        if (draft->appended[i] != bit)
            draft->appended[kept++] = draft->appended[i];
    draft->appended_count = static_cast<u8>(kept);
}

bool guild_engagement_activity_feed(const GuildEngagementDraft& draft, u32 member_count,
                                    bool member_count_known) noexcept {
    const bool enabled = draft.features & guild_feature_activity_feed_enabled;
    if (enabled || (draft.features & guild_feature_activity_feed_disabled))
        return enabled;
    return member_count_known && member_count < kGuildActivityFeedDefaultMembers;
}

void guild_engagement_set_activity_feed(GuildEngagementDraft* draft, bool on) noexcept {
    const u64 add = on ? guild_feature_activity_feed_enabled : guild_feature_activity_feed_disabled;
    const u64 remove = on ? guild_feature_activity_feed_disabled : guild_feature_activity_feed_enabled;
    guild_engagement_add_feature(draft, add);
    guild_engagement_remove_feature(draft, remove);
}

bool guild_engagement_patch(const GuildEngagementDraft& baseline, const GuildEngagementDraft& draft,
                            ConstSlice<u8> stored_names, u16 stored_count, Slice<u8> names,
                            GuildSettingsPatch* patch) noexcept {
    bool timeout = false;
    for (const u32 value : kGuildAfkTimeouts)
        timeout = timeout || value == draft.afk_timeout_seconds;
    if (!patch || draft.default_notifications > 1 || !timeout)
        return false;
    GuildSettingsPatch value{};
    value.fields = guild_settings_patch_afk_channel | guild_settings_patch_afk_timeout |
                   guild_settings_patch_system_channel | guild_settings_patch_default_notifications |
                   guild_settings_patch_system_channel_flags;
    value.afk_channel_id = draft.afk_channel_id;
    value.afk_timeout_seconds = draft.afk_timeout_seconds;
    value.system_channel_id = draft.system_channel_id;
    value.default_message_notifications = draft.default_notifications;
    value.system_channel_flags = draft.system_channel_flags;
    if (guild_engagement_features_changed(baseline, draft)) {
        if (!stored_names.data || !names.data)
            return false;
        // Stored names keep their place unless removed or re-added; appended
        // names follow in the order they were added.
        u32 used = 0, count = 0, cursor = 0, stored = 0;
        Utf8View name{};
        const auto append = [&](Utf8View text) noexcept {
            if (!text.bytes || text.bytes > 255 || 1 + text.bytes > names.count - used)
                return false;
            names[used] = static_cast<u8>(text.bytes);
            copy_bytes(names.data + used + 1, text.data, text.bytes);
            used += 1 + text.bytes;
            ++count;
            return true;
        };
        while (guild_feature_name_next(stored_names, &cursor, &name)) {
            ++stored;
            const u64 bit = guild_feature_bit(name);
            bool appended = false;
            for (u32 i = 0; i < draft.appended_count; ++i)
                appended = appended || draft.appended[i] == bit;
            if (bit && (!(draft.features & bit) || appended))
                continue;
            if (!append(name))
                return false;
        }
        if (cursor != stored_names.count || stored != stored_count)
            return false;
        for (u32 i = 0; i < draft.appended_count; ++i)
            if (!append(guild_feature_name(draft.appended[i])))
                return false;
        value.fields |= guild_settings_patch_features;
        value.feature_names = {names.data, used};
        value.feature_count = static_cast<u16>(count);
    }
    *patch = value;
    return true;
}

void guild_engagement_widget_patch(const GuildEngagementDraft& draft, GuildSettingsPatch* patch) noexcept {
    if (!patch)
        return;
    reset_value(patch);
    patch->fields = guild_settings_patch_widget;
    patch->widget_enabled = draft.widget_enabled;
    patch->widget_channel_id = draft.widget_channel_id;
}

GuildEngagementStep guild_engagement_next_step(const GuildEngagementDraft& baseline,
                                               const GuildEngagementDraft& draft, bool guild_sent) noexcept {
    if (!guild_sent)
        return guild_engagement_equal(baseline, draft) ? GuildEngagementStep::none : GuildEngagementStep::guild;
    return guild_engagement_widget_changed(baseline, draft) ? GuildEngagementStep::widget : GuildEngagementStep::none;
}

GuildRoleDraft guild_role_draft_from(const ChatStateStore& store, const RoleRecord& role) noexcept {
    GuildRoleDraft draft{};
    draft.role_id = role.id;
    draft.permissions = role.permissions;
    draft.color = role.color & 0xffffff;
    draft.hoist = role.hoist;
    draft.mentionable = role.mentionable;
    copy_name(chat_state_store_role_name(store, role), draft.name, &draft.name_bytes);
    return draft;
}

bool guild_role_draft_equal(const GuildRoleDraft& a, const GuildRoleDraft& b) noexcept {
    return a.role_id == b.role_id && a.permissions == b.permissions && a.color == b.color && a.hoist == b.hoist &&
           a.mentionable == b.mentionable && same_bytes(a.name, a.name_bytes, b.name, b.name_bytes);
}

bool guild_role_name_valid(Utf8View text) noexcept {
    return name_valid(text, 1);
}

bool guild_role_patch(const GuildRoleDraft& baseline, const GuildRoleDraft& draft, RolePatch* patch) noexcept {
    if (!patch || baseline.role_id != draft.role_id || !draft.role_id)
        return false;
    RolePatch value{};
    if (!same_bytes(baseline.name, baseline.name_bytes, draft.name, draft.name_bytes)) {
        if (!guild_role_name_valid({draft.name, draft.name_bytes}))
            return false;
        value.fields |= role_patch_name;
        value.name = {draft.name, draft.name_bytes};
    }
    if (baseline.permissions != draft.permissions) {
        value.fields |= role_patch_permissions;
        value.permissions = draft.permissions;
    }
    if (baseline.color != draft.color) {
        value.fields |= role_patch_color;
        value.color = draft.color;
    }
    if (baseline.hoist != draft.hoist) {
        value.fields |= role_patch_hoist;
        value.hoist = draft.hoist;
    }
    if (baseline.mentionable != draft.mentionable) {
        value.fields |= role_patch_mentionable;
        value.mentionable = draft.mentionable;
    }
    if (!value.fields)
        return false;
    *patch = value;
    return true;
}

bool guild_role_toggle(GuildRoleDraft* draft, const GuildSettingsAccess& access, PermissionSet bit,
                       bool enabled) noexcept {
    if (!draft || !bit || (bit & ~kPermissionKnownMask))
        return false;
    // Removing is always allowed on an editable role; granting needs the bit.
    if (enabled && (guild_settings_grantable(access) & bit) != bit)
        return false;
    draft->permissions = enabled ? draft->permissions | bit : draft->permissions & ~bit;
    return true;
}

GuildPermissionLock guild_role_permission_lock(const ChatStateStore& store, const GuildSettingsAccess& access,
                                               const GuildRoleDraft& draft, PermissionSet bit) noexcept {
    if ((access.permissions & bit) != bit)
        return GuildPermissionLock::missing;
    if ((draft.permissions & bit) != bit)
        return GuildPermissionLock::none;
    const PermissionSet without =
        guild_settings_self_permissions_with(store, access, draft.role_id, draft.permissions & ~bit);
    return (without & bit) == bit ? GuildPermissionLock::none : GuildPermissionLock::self;
}

bool guild_role_can_clear(const ChatStateStore& store, const GuildSettingsAccess& access,
                          const GuildRoleDraft& draft) noexcept {
    if (!draft.permissions)
        return false;
    const PermissionSet with = guild_settings_self_permissions_with(store, access, draft.role_id, draft.permissions);
    return guild_settings_self_permissions_with(store, access, draft.role_id, 0) == with;
}

bool guild_settings_order_changed(const ChatStateStore& store, u64 guild_id, ConstSlice<u64> order) noexcept {
    const RoleRecord* stored[kGuildRoleMaximum];
    const u32 count = guild_settings_sorted_roles(store, guild_id, {stored, kGuildRoleMaximum});
    u32 seen = 0;
    for (u32 i = 0; i < order.count; ++i) {
        // Roles deleted since the draft drop out of it.
        if (!chat_state_store_find_role(store, guild_id, order[i]))
            continue;
        if (seen >= count || stored[seen]->id != order[i])
            return true;
        ++seen;
    }
    return seen != count;
}

u32 guild_role_color_format(u32 color, Slice<char> output) noexcept {
    return color_hex_format(color << 8 | 0xFFu, false, output);
}

u32 guild_role_color_rgba(u32 color) noexcept {
    return ((color ? color : 0x99aab5u) & 0xffffffu) << 8 | 0xffu;
}

bool guild_settings_text_matches(const Utf8View& query, const Utf8View& text) noexcept {
    return ascii_contains_ignore_case(text, query);
}

} // namespace maicord
