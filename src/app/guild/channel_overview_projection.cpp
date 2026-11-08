#include "channel_overview_internal.hpp"
#include "../settings/settings.hpp"
#include "maicord/model/guild/permission_compute.hpp"
#include "maicord/model/guild/settings_model.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/intrinsics.hpp"
#include "../account/account_ui_runtime.hpp"
#include "../shell/state.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/guild/member_store.hpp"

namespace maicord::app_detail {
namespace {

static_assert(__is_trivial(ChannelSettingsDraft));

constexpr u64 kDeleteAction = 1;
constexpr u32 slowmode_values[]{0, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 21600};
constexpr Utf8View slowmode_labels[]{
    utf8_literal("Off"),        utf8_literal("5 seconds"),  utf8_literal("10 seconds"),
    utf8_literal("15 seconds"), utf8_literal("30 seconds"), utf8_literal("1 minute"),
    utf8_literal("2 minutes"),  utf8_literal("5 minutes"),  utf8_literal("10 minutes"),
    utf8_literal("15 minutes"), utf8_literal("30 minutes"), utf8_literal("1 hour"),
    utf8_literal("2 hours"),    utf8_literal("6 hours"),    utf8_literal("Current custom value")};
constexpr Utf8View kTriState[]{utf8_literal("Deny"), utf8_literal("Neutral"), utf8_literal("Allow")};

constexpr u32 kTargetBase = 1'000;

// Control ids beyond the text overview's 1–5.
enum Control : u64 {
    control_bitrate = 6,
    control_user_limit,
    control_notice,
    control_sync = 10,
    control_targets,
    control_add_target,
    control_grid,
    control_group = 100,
    control_permission = 200,
};

} // namespace
namespace channel_overview_detail {

Overview* owner;
u64 lifetime;

const ChatStateStore& state() noexcept {
    return g_app.account_ui->chat_state;
}
ChannelOverviewScope scope(u64 id) noexcept {
    if (!g_app.account_ui)
        return {};
    const auto* channel = chat_state_store_find_channel(state(), id);
    return channel ? ChannelOverviewScope{g_app.plugin_account_scope_serial,
                                          g_app.account.status.profile_id,
                                          state().self_user_id,
                                          channel->guild_id,
                                          channel->id,
                                          channel->generation}
                   : ChannelOverviewScope{};
}

} // namespace channel_overview_detail
namespace {

// Channel-level permissions of the current account.
PermissionSet self_permissions(u64 channel_id) noexcept {
    const auto* channel = chat_state_store_find_channel(state(), channel_id);
    if (!channel || !channel->guild_id)
        return 0;
    u64 roles[kGuildRoleMaximum];
    const u32 count = chat_state_store_self_role_ids(state(), channel->guild_id, {roles, kGuildRoleMaximum});
    return chat_state_store_member_permissions(state(), channel->guild_id, channel_id,
                                                  {state().self_user_id, {roles, count}, 0},
                                                  {0, state().self_mfa == SelfMfaState::disabled});
}
bool supported_type(u8 type) noexcept {
    return type == 0 || type == 5 || type == 15 || channel_settings_supported(type);
}
bool channel_usable(u64 id) noexcept {
    if (!g_app.account_ui)
        return false;
    const auto* c = chat_state_store_find_channel(state(), id);
    return c && c->occupied && !(c->flags & kChannelObfuscatedFlag) && c->guild_id && supported_type(c->type) &&
           c->permissions_resolved && (c->effective_permissions & kPermissionViewChannel);
}

} // namespace
namespace channel_overview_detail {

// Text overview through the overview controller.
bool manage(u64 id) noexcept {
    if (!g_app.account_ui)
        return false;
    const auto* c = chat_state_store_find_channel(state(), id);
    constexpr u64 required = kPermissionViewChannel | kPermissionManageChannels;
    constexpr u32 actions = channel_action_view | channel_action_manage_channel;
    return c && c->occupied && !(c->flags & kChannelObfuscatedFlag) && c->guild_id && c->type == 0 &&
           c->permissions_resolved && (c->effective_permissions & required) == required &&
           (c->actions & actions) == actions;
}
bool manage_channel(u64 id) noexcept {
    return channel_usable(id) && permission_has(self_permissions(id), kPermissionManageChannels);
}
bool manage_overrides(u64 id) noexcept {
    return channel_usable(id) && permission_has(self_permissions(id), kPermissionManageRoles);
}
bool available() noexcept {
    return g_app.account_ui && !g_app.login.visible && g_app.account.status.running;
}

} // namespace channel_overview_detail
namespace {

bool connected() noexcept {
    return g_app.account.instance && g_app.account.status.running && g_app.account.status.gateway_ready &&
           g_app.account.api.guild_admin;
}
bool busy() noexcept {
    return owner->queued || owner->request_id;
}
bool selected_permissions() noexcept {
    const auto* page = ui_page_registry_selected(owner->registry);
    return page && ui_page_key_equal(page->key, permissions_key);
}

} // namespace
namespace channel_overview_detail {

bool editable() noexcept {
    if (!owner || !owner->scene.active || owner->layer.choice || g_app.text_form || busy())
        return false;
    if (owner->text && !selected_permissions())
        return owner->controller.active && owner->controller.available && owner->controller.can_manage &&
               owner->controller.authoritative && !owner->controller.request_id && !owner->controller.queued;
    return (channel_settings_supported(owner->type) || selected_permissions()) &&
           scope(owner->identity.channel_id).channel_id != 0;
}

} // namespace channel_overview_detail
namespace {

bool overview_dirty() noexcept {
    if (owner->text)
        return owner->controller.dirty_fields != 0;
    return owner->loaded && !channel_settings_equal(owner->base, owner->draft);
}
bool overwrites_dirty() noexcept {
    return !permission_overwrite_draft_equal(owner->overwrite_draft,
                                             {owner->overwrite_base.entries, owner->overwrite_base.count});
}
bool page_dirty() noexcept {
    return selected_permissions() ? overwrites_dirty() : overview_dirty();
}
bool binding_ready(void* model, u64 identity) noexcept {
    return Editors::ready(model, identity) &&
           channel_overview_scope_equal(scope(owner->identity.channel_id), owner->identity);
}

} // namespace
namespace channel_overview_detail {

bool import_models() noexcept {
    application_composer_text_input_suspend();
    if (!owner->text) {
        return message_composer_refresh_borrowed(&owner->models[0], owner->draft.name_bytes) &&
               bounded_edit_bind(&owner->bindings[0], &owner->models[0], kGuildAdminNameCodepoints, false,
                                 owner->lifetime, binding_ready, &owner->models[0]);
    }
    const u32 lengths[]{owner->controller.draft.name_bytes, owner->controller.draft.topic_bytes};
    for (u32 i = 0; i < 2; ++i) {
        if (!message_composer_refresh_borrowed(&owner->models[i], lengths[i]) ||
            !bounded_edit_bind(&owner->bindings[i], &owner->models[i], i ? 1024 : 100, i != 0, owner->lifetime,
                               binding_ready, &owner->models[i]))
            return false;
    }
    return true;
}
Utf8View status() noexcept {
    const auto& c = owner->controller;
    if (owner->failed && owner->status.bytes)
        return owner->status;
    if (selected_permissions() || !owner->text) {
        if (owner->request_id && owner->op != Op::fetch)
            return utf8_literal("Saving changes…");
        return utf8_literal("Careful — you have unsaved changes!");
    }
    if (c.phase == ChannelOverviewPhase::permission_lost)
        return utf8_literal("You no longer have permission to edit this channel.");
    if (!c.available)
        return utf8_literal("Reconnect to edit this channel. Your changes are kept.");
    if (c.phase == ChannelOverviewPhase::loading || c.phase == ChannelOverviewPhase::refetch)
        return utf8_literal("Loading…");
    if (c.phase == ChannelOverviewPhase::saving)
        return utf8_literal("Saving changes…");
    if (c.phase == ChannelOverviewPhase::conflict)
        return utf8_literal("Someone else changed this channel. Review your changes before saving.");
    if (c.failure == ChannelOverviewFailure::input)
        return utf8_literal("Channel name must contain 1–100 characters.");
    if (c.failure != ChannelOverviewFailure::none)
        return utf8_literal("Couldn't confirm the save. Your changes are kept.");
    return c.dirty_fields ? utf8_literal("Careful — you have unsaved changes!") : Utf8View{};
}

} // namespace channel_overview_detail
namespace {

constexpr UiSettingsControlPresentation kCriticalRow{UiSettingsVariant::critical, 0, kUiSettingsNoDefault};
// Appends a control and returns it for the optional fields; past capacity the row lands in a scratch sink.
UiSettingsControl& add_control(u64 id, const Utf8View& label, const Utf8View& description, UiSettingsControlKind kind,
                               bool enabled = true, bool checked = false,
                               UiSettingsControlPresentation presentation = {}) noexcept {
    static UiSettingsControl overflow;
    if (owner->control_count >= kControlCapacity)
        return overflow;
    const u32 index = owner->control_count++;
    owner->controls[index] = {id, label, description, kind, enabled, checked, {}, 0, {}, nullptr};
    owner->presentation[index] = presentation;
    reset_value(&owner->avatar_media[index]);
    return owner->controls[index];
}
u32 slowmode_index(u32 seconds) noexcept {
    for (u32 i = 0; i < 14; ++i)
        if (slowmode_values[i] == seconds)
            return i;
    return 14;
}
void project_text_overview() noexcept {
    auto& p = *owner;
    const bool edit = editable();
    add_control(1, utf8_literal("Channel name"), {}, UiSettingsControlKind::edit, edit).editor =
        Editors::view(0, 1, utf8_literal("Channel name"), edit);
    add_control(2, utf8_literal("Channel topic"), {}, UiSettingsControlKind::multiline, edit).editor =
        Editors::view(1, 2, utf8_literal("Tell members what this channel is for"), edit);
    auto& slowmode = add_control(3, utf8_literal("Slowmode"),
                                 utf8_literal("Members can send one message per interval unless they can bypass slowmode."),
                                 UiSettingsControlKind::choice, edit);
    slowmode.choices = {slowmode_labels, 15};
    slowmode.selected_choice = slowmode_index(p.controller.draft.slowmode_seconds);
    add_control(4, utf8_literal("Age-restricted channel"),
                utf8_literal("Members must confirm they meet the age requirement to view this channel."),
                UiSettingsControlKind::toggle, edit, p.controller.draft.nsfw);
    const bool conflict = p.controller.phase == ChannelOverviewPhase::conflict;
    add_control(5, conflict ? utf8_literal("Review your changes") : utf8_literal("Refresh channel settings"),
                conflict ? utf8_literal("Keep your changes on top of the latest settings.")
                         : utf8_literal("Reload the latest settings. Unsaved changes are kept."),
                UiSettingsControlKind::action,
                p.controller.available && p.controller.can_manage && !p.controller.request_id && !p.controller.queued,
                false, {conflict ? UiSettingsVariant::primary : UiSettingsVariant::standard, 0, 0})
        .action_label = conflict ? utf8_literal("Use my changes") : utf8_literal("Refresh");
}
void project_generic_overview() noexcept {
    auto& p = *owner;
    const bool edit = editable() && p.loaded && p.can_overview;
    auto& d = p.draft;
    if (p.fetch_failed)
        add_control(control_notice, utf8_literal("Couldn't load this channel"), p.status, UiSettingsControlKind::notice,
                    true, false, kCriticalRow)
            .action_label = utf8_literal("Try again");
    else if (!p.loaded)
        add_control(control_notice, {}, utf8_literal("Loading…"), UiSettingsControlKind::notice);
    else if (p.conflict)
        add_control(control_notice, utf8_literal("Changed elsewhere"),
                    utf8_literal("Someone else updated this channel. Load the latest values or keep editing."),
                    UiSettingsControlKind::notice, true, false, kCriticalRow)
            .action_label = utf8_literal("Load latest");
    add_control(1, p.type == 4 ? utf8_literal("Category name") : utf8_literal("Channel name"), {},
                UiSettingsControlKind::edit, edit)
        .editor =
        Editors::view(0, 1, p.type == 4 ? utf8_literal("Category name") : utf8_literal("Channel name"), edit);
    if (p.type == 4)
        return;
    // Bitrate marks: the standard steps plus the stored value when higher.
    p.bitrate_count = 0;
    Bank text{p.bitrate_text, sizeof(p.bitrate_text), 0};
    u32 selected = 0;
    for (const u32 value : kChannelSettingsBitrates) {
        const u32 start = text.used;
        text.number(value / 1000);
        p.bitrate_labels[p.bitrate_count] = {text.data + start, text.used - start};
        if (value <= d.bitrate)
            selected = p.bitrate_count;
        p.bitrate_values[p.bitrate_count++] = value;
    }
    if (d.bitrate > kChannelSettingsBitrates[7] && p.bitrate_count < 16) {
        const u32 start = text.used;
        text.number(d.bitrate / 1000);
        p.bitrate_labels[p.bitrate_count] = {text.data + start, text.used - start};
        selected = p.bitrate_count;
        p.bitrate_values[p.bitrate_count++] = d.bitrate;
    }
    auto& bitrate = add_control(control_bitrate, utf8_literal("Bitrate (kbps)"),
                                utf8_literal("Higher bitrates sound better but use more bandwidth."),
                                UiSettingsControlKind::slider, edit, false,
                                {UiSettingsVariant::standard, ui_settings_presentation_marks, 5});
    bitrate.choices = {p.bitrate_labels, p.bitrate_count};
    bitrate.selected_choice = selected;
    if (p.type == 2) {
        auto& limit = add_control(control_user_limit, utf8_literal("User limit"),
                                  utf8_literal("How many members can join at once. Members who can move others ignore the limit."),
                                  UiSettingsControlKind::slider, edit);
        limit.choices = {p.user_limit_labels, kChannelSettingsUserLimitMaximum + 1};
        limit.selected_choice = minimum(d.user_limit, kChannelSettingsUserLimitMaximum);
    }
    auto& slowmode = add_control(3, utf8_literal("Slowmode"),
                                 utf8_literal("Members can send one message per interval unless they can bypass slowmode."),
                                 UiSettingsControlKind::choice, edit);
    slowmode.choices = {slowmode_labels, 15};
    slowmode.selected_choice = slowmode_index(d.slowmode_seconds);
    add_control(4, utf8_literal("Age-restricted channel"),
                utf8_literal("Members must confirm they meet the age requirement to view this channel."),
                UiSettingsControlKind::toggle, edit, d.nsfw);
}

Utf8View target_name(Bank& bank, const PermissionOverwriteRecord& entry, u32* color, bool* member) noexcept {
    *color = 0;
    *member = entry.type == 1;
    if (entry.type == 0) {
        if (entry.id == owner->identity.guild_id)
            return utf8_literal("@everyone");
        const auto* r = chat_state_store_find_role(state(), owner->identity.guild_id, entry.id);
        if (!r)
            return utf8_literal("Deleted role");
        *color = guild_role_color_rgba(r->color);
        return bank.add(chat_state_store_role_name(state(), *r));
    }
    const auto& users = g_app.account_ui->user_store;
    if (const auto* m = guild_member_store_find(g_app.account_ui->guild_member_store, owner->identity.guild_id,
                                                        entry.id)) {
        const auto nick = guild_member_store_nickname(*m);
        if (nick.bytes)
            return bank.add(nick);
    }
    if (const auto* user = user_store_find(users, entry.id)) {
        const auto name = user_store_name(users, *user);
        if (name.bytes)
            return bank.add(name);
    }
    // Uncached members show their user ID.
    const u32 start = bank.used;
    bank.add("Member ");
    if (entry.id)
        bank.number(entry.id);
    return bank.since(start);
}
// Display order: @everyone, roles by hierarchy, then members.
u32 target_order(u32* order) noexcept {
    const auto& d = owner->overwrite_draft;
    u32 count = 0;
    for (u32 i = 0; i < d.count; ++i)
        order[count++] = i;
    const u64 guild = owner->identity.guild_id;
    heap_sort_shared(order, count, [&](u32 a, u32 b) noexcept {
        const auto ea = d.entries[a], eb = d.entries[b];
        const int ra = ea.id == guild ? 0 : ea.type == 0 ? 1 : 2, rb = eb.id == guild ? 0 : eb.type == 0 ? 1 : 2;
        if (ra != rb)
            return ra < rb;
        if (ra == 1) {
            const auto* x = chat_state_store_find_role(state(), guild, ea.id);
            const auto* y = chat_state_store_find_role(state(), guild, eb.id);
            const u32 px = x ? x->position : 0, py = y ? y->position : 0;
            if (px != py)
                return px > py;
        }
        return ea.id < eb.id;
    });
    return count;
}
const ChannelRecord* parent_channel() noexcept {
    const auto* channel = chat_state_store_find_channel(state(), owner->identity.channel_id);
    return channel && channel->parent_id ? chat_state_store_find_channel(state(), channel->parent_id) : nullptr;
}
void project_permissions(Bank& bank) noexcept {
    auto& p = *owner;
    const bool edit = editable() && p.can_permissions;
    if (p.failed && p.status.bytes)
        add_control(control_notice, {}, p.status, UiSettingsControlKind::notice, true, false, kCriticalRow);
    if (p.overwrite_conflict)
        add_control(control_notice, utf8_literal("Changed elsewhere"),
                    utf8_literal("Someone else changed these permissions. Load the latest values or keep editing."),
                    UiSettingsControlKind::notice, true, false, kCriticalRow)
            .action_label = utf8_literal("Load latest");
    if (const auto* parent = parent_channel()) {
        const bool synced = permission_overwrite_draft_equal(p.overwrite_draft,
                                                             chat_state_store_channel_overwrites(state(), *parent));
        const u32 start = bank.used;
        bank.add(synced ? utf8_literal("Synced with ") : utf8_literal("Not synced with "));
        bank.add(chat_state_store_channel_name(state(), *parent));
        add_control(control_sync, Utf8View{bank.data + start, bank.used - start},
                    synced ? utf8_literal("This channel uses its category's permissions.")
                           : utf8_literal("Sync to copy the category's permissions to this channel."),
                    UiSettingsControlKind::notice, edit || synced, false,
                    {synced ? UiSettingsVariant::positive : UiSettingsVariant::standard, 0, kUiSettingsNoDefault})
            .action_label = synced ? Utf8View{} : utf8_literal("Sync now");
    }
    add_control(control_targets, utf8_literal("Roles and members"),
                utf8_literal("Overrides replace role permissions for this channel. Pick one to edit it."),
                UiSettingsControlKind::category);
    add_control(control_add_target, utf8_literal("Add a role or member"), {}, UiSettingsControlKind::action, edit)
        .action_label = utf8_literal("Add");
    // @everyone is always listed, even without an explicit override.
    if (permission_overwrite_draft_find(p.overwrite_draft, p.identity.guild_id) == ~0u) {
        add_control(kTargetBase + kPermissionOverwriteDraftMaximum, utf8_literal("@everyone"), utf8_literal("Role"),
                    UiSettingsControlKind::action, true, false,
                    {UiSettingsVariant::standard,
                     static_cast<u8>(ui_settings_presentation_compact | ui_settings_presentation_accent |
                                     (p.selected_target == p.identity.guild_id ? ui_settings_presentation_selected : 0)),
                     kUiSettingsNoDefault});
        p.row_accent[kPermissionOverwriteDraftMaximum] = guild_role_color_rgba(0);
    }
    u32 order[kPermissionOverwriteDraftMaximum];
    const u32 count = target_order(order);
    for (u32 i = 0; i < count; ++i) {
        const auto entry = p.overwrite_draft.entries[order[i]];
        u32 color = 0;
        bool member = false;
        const auto name = target_name(bank, entry, &color, &member);
        const u32 overrides = population_count(entry.allow | entry.deny);
        const u32 start = bank.used;
        bank.add(member ? utf8_literal("Member") : utf8_literal("Role"));
        if (overrides) {
            bank.add(" · ");
            bank.number(overrides);
            bank.add(overrides == 1 ? utf8_literal(" override") : utf8_literal(" overrides"));
        }
        u8 flags = ui_settings_presentation_compact;
        if (entry.id == p.selected_target)
            flags |= ui_settings_presentation_selected;
        if (member)
            flags |= ui_settings_presentation_avatar;
        const u32 target = p.control_count;
        add_control(kTargetBase + order[i], name, Utf8View{bank.data + start, bank.used - start},
                    UiSettingsControlKind::action, true, false,
                    {UiSettingsVariant::standard, static_cast<u8>(flags | ui_settings_presentation_accent),
                     kUiSettingsNoDefault})
            .action_label = entry.id == p.identity.guild_id || !edit ? Utf8View{} : utf8_literal("Remove");
        // Member targets draw the cached avatar over their initials once resident.
        if (member && target < kControlCapacity)
            p.avatar_media[target] = application_user_avatar_media(entry.id);
        p.row_accent[order[i]] = member ? guild_person_color(entry.id) : (color ? color : guild_role_color_rgba(0));
    }
    // Grid for the selected target.
    const u32 selected = permission_overwrite_draft_find(p.overwrite_draft, p.selected_target);
    const PermissionSet allow = selected != ~0u ? p.overwrite_draft.entries[selected].allow : 0;
    const PermissionSet deny = selected != ~0u ? p.overwrite_draft.entries[selected].deny : 0;
    PermissionOverwriteRecord shown{p.selected_target, allow, deny,
                                           static_cast<u8>(selected != ~0u ? p.overwrite_draft.entries[selected].type : 0)};
    u32 color = 0;
    bool member = false;
    const auto name = target_name(bank, shown, &color, &member);
    const u32 start = bank.used;
    bank.add("Permissions for ");
    bank.add(name);
    add_control(control_grid, Utf8View{bank.data + start, bank.used - start},
                utf8_literal("Deny, keep neutral to inherit, or allow each permission."), UiSettingsControlKind::category);
    const u8 scope_bits = permission_grid_channel_scope(p.type);
    const PermissionSet mine = self_permissions(p.identity.channel_id);
    const auto* guild = chat_state_store_find_guild(state(), p.identity.guild_id);
    const PermissionSet grantable =
        (guild && guild->owner_id == state().self_user_id) || (mine & kPermissionAdministrator) ? kPermissionKnownMask
                                                                                               : mine;
    const auto entries = permission_grid_entries();
    for (u32 group = 0; group < static_cast<u32>(PermissionGroup::count); ++group) {
        bool first = true;
        for (u32 i = 0; i < entries.count; ++i) {
            const auto& e = entries[i];
            if (static_cast<u32>(e.group) != group || !permission_grid_applies(e, scope_bits))
                continue;
            if (first) {
                add_control(control_group + group, permission_group_label(static_cast<PermissionGroup>(group)), {},
                            UiSettingsControlKind::category);
                first = false;
            }
            const auto value = permission_tri_get(allow, deny, e.bit);
            auto& row = add_control(control_permission + i, permission_grid_label(e, scope_bits),
                                    permission_grid_description(e, scope_bits), UiSettingsControlKind::segments,
                                    edit && ((grantable & e.bit) == e.bit || value != PermissionTriState::neutral),
                                    false,
                                    {UiSettingsVariant::standard, ui_settings_presentation_tristate, kUiSettingsNoDefault});
            row.choices = {kTriState, 3};
            row.selected_choice = static_cast<u32>(value);
        }
    }
}

bool footer_can_save() noexcept {
    auto& p = *owner;
    if (!editable() || busy())
        return false;
    if (selected_permissions())
        return p.can_permissions && overwrites_dirty() && !p.overwrite_conflict && connected();
    if (p.text) {
        const bool conflict = p.controller.phase == ChannelOverviewPhase::conflict;
        return p.controller.dirty_fields && !conflict &&
               channel_overview_patch_valid({channel_overview_name_view(p.controller.draft),
                                             channel_overview_topic_view(p.controller.draft),
                                             p.controller.draft.slowmode_seconds, channel_overview_all,
                                             p.controller.draft.nsfw});
    }
    ChannelPatch patch{};
    return p.can_overview && !p.conflict && connected() && channel_settings_patch(p.base, p.draft, &patch);
}
bool resolve_color(void*, u64 id, u32 choice, BackgroundTheme* theme) noexcept {
    if (!owner || !theme || choice != kUiSettingsAccentChoice || id < kTargetBase ||
        id > kTargetBase + kPermissionOverwriteDraftMaximum)
        return false;
    const u32 rgba = owner->row_accent[id - kTargetBase];
    if (!rgba)
        return false;
    reset_value(theme);
    theme->colors[0] = rgba >> 8;
    theme->color_count = 1;
    return true;
}

} // namespace
namespace channel_overview_detail {

bool project() noexcept {
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    p.bank_index ^= 1;
    Bank bank{p.banks[p.bank_index], kBankBytes, 0};
    p.control_count = 0;
    const bool permissions = selected_permissions();
    if (permissions)
        project_permissions(bank);
    else if (p.text)
        project_text_overview();
    else
        project_generic_overview();
    const bool dirty = page_dirty();
    const bool saving = permissions || !p.text ? p.request_id && p.op != Op::fetch
                                               : p.controller.phase == ChannelOverviewPhase::saving;
    const bool failed = permissions || !p.text ? p.failed : p.controller.failure != ChannelOverviewFailure::none;
    const bool conflict = !permissions && p.text && p.controller.phase == ChannelOverviewPhase::conflict;
    p.footer = {status(), utf8_literal("Save changes"), utf8_literal("Reset"),
                dirty || saving || conflict, footer_can_save(), saving, failed};
    auto& active = permissions ? p.permissions_content : p.content;
    auto& other = permissions ? p.content : p.permissions_content;
    active.controls = {p.controls, p.control_count};
    active.presentation = p.presentation;
    active.avatar_media = p.avatar_media;
    active.footer = &p.footer;
    reset_value(&other.controls);
    other.presentation = nullptr;
    other.avatar_media = nullptr;
    other.footer = nullptr;
    ++active.revision;
    p.projected_revision = p.controller.revision;
    ui_settings_scene_set_focus_visible(&p.scene, g_app.keyboard_focus_visible);
    ui_settings_scene_bind_palette(&p.scene, {owner, resolve_color, ++p.palette_revision});
    return ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips());
}
bool repaint() noexcept {
    return settings_modal_present(project());
}
void build_navigation() noexcept {
    auto& p = *owner;
    const auto* channel = chat_state_store_find_channel(state(), p.identity.channel_id);
    Bank header{p.header, sizeof(p.header), 0};
    if (channel) {
        if (p.type == 0 || p.type == 5 || p.type == 15)
            header.add("#");
        header.add(chat_state_store_channel_name(state(), *channel));
        for (u32 i = 0; i < header.used; ++i)
            if (p.header[i] >= 'a' && p.header[i] <= 'z')
                p.header[i] = static_cast<char>(p.header[i] - 32);
    }
    p.nav_count = 0;
    p.nav[p.nav_count++] = {UiSettingsNavKind::header, false, UiIcon::count,
                            header.used ? Utf8View{p.header, header.used} : utf8_literal("Channel"), {}, 0};
    if (p.can_overview)
        p.nav[p.nav_count++] = {UiSettingsNavKind::page, false, UiIcon::settings, utf8_literal("Overview"), page_key, 0};
    if (p.can_permissions)
        p.nav[p.nav_count++] = {UiSettingsNavKind::page, false, UiIcon::lock, utf8_literal("Permissions"),
                                permissions_key, 0};
    if (p.can_delete) {
        p.nav[p.nav_count++] = {UiSettingsNavKind::separator, false, UiIcon::count, {}, {}, 0};
        p.nav[p.nav_count++] = {UiSettingsNavKind::action, true, UiIcon::trash,
                                p.type == 4 ? utf8_literal("Delete category") : utf8_literal("Delete channel"), {},
                                kDeleteAction};
    }
    ui_settings_scene_bind_navigation(&p.scene, {p.nav, p.nav_count}, ++p.nav_revision);
}

void load_overwrites() noexcept {
    const auto* channel = chat_state_store_find_channel(state(), owner->identity.channel_id);
    const auto stored = channel ? chat_state_store_channel_overwrites(state(), *channel)
                                : ConstSlice<PermissionOverwriteRecord>{};
    permission_overwrite_draft_load(&owner->overwrite_base, stored);
    permission_overwrite_draft_load(&owner->overwrite_draft, stored);
    owner->overwrite_conflict = owner->overwrite_echo = false;
    if (permission_overwrite_draft_find(owner->overwrite_draft, owner->selected_target) == ~0u)
        owner->selected_target = owner->identity.guild_id;
}
void queue(Op op) noexcept {
    owner->op = op;
    owner->queued = true;
    owner->retry_at = 0;
    owner->failed = false;
    reset_value(&owner->status);
    application_request_frame();
}

} // namespace channel_overview_detail
namespace {

bool guard_dirty() noexcept {
    if (!page_dirty())
        return false;
    settings_layer_shake(&owner->layer);
    return true;
}

} // namespace
namespace channel_overview_detail {

bool form_valid(TextFormOwnerKey key) noexcept {
    return owner && owner->scene.active && key.scope == owner->lifetime && key.item == owner->identity.channel_id &&
           channel_overview_scope_equal(scope(key.item), owner->identity);
}

} // namespace channel_overview_detail
namespace {

bool form_delete(TextFormOwnerKey key, Utf8View) noexcept {
    if (!form_valid(key) || busy() || !owner->can_delete)
        return false;
    application_text_form_close(false, false);
    queue(Op::remove);
    repaint();
    return true;
}
void confirm_delete() noexcept {
    if (!owner->can_delete || busy() || g_app.text_form)
        return;
    const auto* channel = chat_state_store_find_channel(state(), owner->identity.channel_id);
    if (!channel)
        return;
    static char title[kGuildAdminNameBytes + 32];
    Bank bank{title, sizeof(title), 0};
    bank.add("Delete ");
    if (owner->type == 0 || owner->type == 5 || owner->type == 15)
        bank.add("#");
    bank.add(chat_state_store_channel_name(state(), *channel));
    bank.add("?");
    ApplicationTextFormRequest request{};
    request.title = {title, bank.used};
    request.context = owner->type == 4 ? utf8_literal("Channels in this category are kept. This can't be undone.")
                                       : utf8_literal("Its messages are deleted for everyone. This can't be undone.");
    request.accept = owner->type == 4 ? utf8_literal("Delete category") : utf8_literal("Delete channel");
    request.owner = {owner->lifetime, 3, owner->identity.channel_id};
    request.valid = form_valid;
    request.save = form_delete;
    request.confirmation_only = true;
    request.danger = true;
    application_text_form_open(request);
}
void open_slowmode(const UiRect& anchor, u32 current) noexcept {
    for (u32 i = 0; i < 14; ++i)
        popup_item_set(&owner->popup_items[i], slowmode_labels[i], 700 + i,
                       static_cast<u16>(popup_item_enabled | (slowmode_values[i] == current ? popup_item_checked : 0)));
    owner->menu = Menu::slowmode;
    settings_layer_open_choice(&owner->layer, owner->identity.channel_id, {owner->popup_items, 14}, anchor, 240);
}
void open_target_menu(UiRect anchor) noexcept {
    auto& p = *owner;
    Bank bank{p.popup_text, sizeof(p.popup_text), 0};
    u32 count = 0;
    const RoleRecord* roles[kGuildRoleMaximum];
    const u32 role_count = guild_settings_sorted_roles(state(), p.identity.guild_id, {roles, kGuildRoleMaximum});
    for (u32 i = 0; i < role_count && count < kPopupCapacity; ++i) {
        if (roles[i]->id == p.identity.guild_id || permission_overwrite_draft_find(p.overwrite_draft, roles[i]->id) != ~0u)
            continue;
        popup_item_set(&p.popup_items[count], bank.add(chat_state_store_role_name(state(), *roles[i])), 2'000 + count,
                       popup_item_enabled);
        p.popup_items[count].detail = utf8_literal("Role");
        p.menu_ids[count] = roles[i]->id;
        p.menu_types[count++] = 0;
    }
    const auto& members = g_app.account_ui->guild_member_store;
    const auto& users = g_app.account_ui->user_store;
    bool separated = false;
    for (u32 i = 0; i < members.slot_extent && count < kPopupCapacity; ++i) {
        const auto& m = members.records[i];
        if (!m.occupied || m.guild_id != p.identity.guild_id ||
            permission_overwrite_draft_find(p.overwrite_draft, m.user_id) != ~0u)
            continue;
        Utf8View name = guild_member_store_nickname(m);
        if (!name.bytes)
            if (const auto* user = user_store_find(users, m.user_id))
                name = user_store_name(users, *user);
        if (!name.bytes)
            continue;
        popup_item_set(&p.popup_items[count], bank.add(name), 2'000 + count,
                       static_cast<u16>(popup_item_enabled | (!separated && count ? popup_item_separator_before : 0)));
        p.popup_items[count].detail = utf8_literal("Member");
        separated = true;
        p.menu_ids[count] = m.user_id;
        p.menu_types[count++] = 1;
    }
    if (!count)
        return;
    p.menu = Menu::target;
    settings_layer_open_choice(&p.layer, p.identity.channel_id, {p.popup_items, count}, anchor, 280);
}
void set_override(u64 id, PermissionSet bit, PermissionTriState value) noexcept {
    auto& d = owner->overwrite_draft;
    if (permission_overwrite_draft_find(d, id) == ~0u && !permission_overwrite_draft_add(&d, id, 0))
        return;
    permission_overwrite_draft_set(&d, id, bit, value);
    // An implicit @everyone entry that returns to neutral is dropped again.
    const u32 index = permission_overwrite_draft_find(d, id);
    if (id == owner->identity.guild_id && index != ~0u && !d.entries[index].allow && !d.entries[index].deny &&
        permission_overwrite_draft_find(owner->overwrite_base, id) == ~0u)
        permission_overwrite_draft_remove(&d, id);
}

void request_close() noexcept;
void select(UiPageKey key) noexcept {
    auto& p = *owner;
    if (guard_dirty())
        return;
    settings_layer_close_choice(&p.layer);
    p.failed = false;
    reset_value(&p.status);
    if (ui_page_key_equal(key, permissions_key))
        load_overwrites();
    ui_page_registry_select(&p.registry, key);
    project();
    ui_settings_scene_focus(&p.scene, {UiSettingsTargetKind::page, key, 0});
    repaint();
}

} // namespace
namespace channel_overview_detail {

void activate_hook(SettingsLayer*, const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    auto& c = p.controller;
    const auto target = activation.value.target;
    if (target.kind == UiSettingsTargetKind::close) {
        request_close();
        return;
    }
    if (target.kind == UiSettingsTargetKind::page) {
        if (!ui_page_key_equal(target.page, p.scene.selected_page))
            select(target.page);
        return;
    }
    if (target.kind == UiSettingsTargetKind::action) {
        if (target.control_id == kDeleteAction && !guard_dirty())
            confirm_delete();
        return;
    }
    const bool permissions = selected_permissions();
    if (target.kind == UiSettingsTargetKind::save) {
        if (!footer_can_save())
            return;
        if (permissions)
            queue(Op::overwrites);
        else if (p.text)
            channel_overview_controller_save(&c);
        else
            queue(Op::update);
        application_composer_text_input_suspend();
        repaint();
        return;
    }
    if (target.kind == UiSettingsTargetKind::reset) {
        if (busy())
            return;
        p.failed = false;
        reset_value(&p.status);
        if (permissions)
            load_overwrites();
        else if (p.text) {
            if (channel_overview_controller_reset(&c))
                import_models();
        } else {
            copy_bytes(&p.draft, &p.base, sizeof(p.draft));
            p.conflict = false;
            import_models();
        }
        repaint();
        return;
    }
    if (target.kind != UiSettingsTargetKind::control)
        return;
    const u64 id = target.control_id;
    if (!permissions && (id == 1 || (id == 2 && p.text))) {
        settings_layer_focus(&p.layer, target);
        return;
    }
    if (id == control_notice) {
        if (permissions && p.overwrite_conflict)
            load_overwrites();
        else if (!p.text && p.fetch_failed) {
            p.fetch_failed = false;
            queue(Op::fetch);
        } else if (!p.text && p.conflict) {
            p.loaded = false;
            p.conflict = false;
            queue(Op::fetch);
        }
        repaint();
        return;
    }
    if (!permissions && p.text && id == 5) {
        if (c.phase == ChannelOverviewPhase::conflict)
            channel_overview_controller_rebase(&c);
        else
            channel_overview_controller_reload(&c);
        repaint();
        return;
    }
    if (!editable())
        return;
    if (permissions) {
        if (id == control_sync) {
            if (const auto* parent = parent_channel())
                permission_overwrite_draft_load(&p.overwrite_draft, chat_state_store_channel_overwrites(state(), *parent));
            if (permission_overwrite_draft_find(p.overwrite_draft, p.selected_target) == ~0u)
                p.selected_target = p.identity.guild_id;
        } else if (id == control_add_target)
            open_target_menu(activation.anchor);
        else if (id >= kTargetBase && id <= kTargetBase + kPermissionOverwriteDraftMaximum) {
            const u32 index = static_cast<u32>(id - kTargetBase);
            const u64 selected = index < p.overwrite_draft.count ? p.overwrite_draft.entries[index].id
                                                                 : p.identity.guild_id;
            if (activation.button && selected != p.identity.guild_id) {
                permission_overwrite_draft_remove(&p.overwrite_draft, selected);
                if (p.selected_target == selected)
                    p.selected_target = p.identity.guild_id;
            } else
                p.selected_target = selected;
        } else if (id >= control_permission && id < control_permission + permission_grid_entries().count &&
                   activation.value.selected_choice <= 2)
            set_override(p.selected_target, permission_grid_entries()[static_cast<u32>(id - control_permission)].bit,
                         static_cast<PermissionTriState>(activation.value.selected_choice));
        repaint();
        return;
    }
    if (p.text) {
        if (id == 4) {
            channel_overview_controller_edit(&c, {{}, {}, 0, channel_overview_nsfw, !c.draft.nsfw});
            ui_settings_scene_animate_toggle(&p.scene, target, c.draft.nsfw, clock_monotonic_milliseconds());
            repaint();
        } else if (id == 3)
            open_slowmode(activation.anchor, c.draft.slowmode_seconds);
        return;
    }
    if (!p.loaded || !p.can_overview)
        return;
    if (id == 4) {
        p.draft.nsfw = activation.value.checked;
        ui_settings_scene_animate_toggle(&p.scene, target, p.draft.nsfw, clock_monotonic_milliseconds());
    } else if (id == 3)
        open_slowmode(activation.anchor, p.draft.slowmode_seconds);
    else if (id == control_bitrate && activation.value.selected_choice < p.bitrate_count)
        p.draft.bitrate = p.bitrate_values[activation.value.selected_choice];
    else if (id == control_user_limit && activation.value.selected_choice <= kChannelSettingsUserLimitMaximum)
        p.draft.user_limit = activation.value.selected_choice;
    repaint();
}
void choose_hook(SettingsLayer*, u32 action) noexcept {
    auto& p = *owner;
    const Menu menu = p.menu;
    p.menu = Menu::none;
    if (!action || !editable())
        return;
    if (menu == Menu::slowmode && action >= 700 && action < 714) {
        if (p.text)
            channel_overview_controller_edit(&p.controller,
                                             {{}, {}, slowmode_values[action - 700], channel_overview_slowmode, false});
        else
            p.draft.slowmode_seconds = slowmode_values[action - 700];
    } else if (menu == Menu::target && action >= 2'000 && action < 2'000 + kPopupCapacity) {
        const u32 index = action - 2'000;
        if (permission_overwrite_draft_add(&p.overwrite_draft, p.menu_ids[index], p.menu_types[index]))
            p.selected_target = p.menu_ids[index];
    }
}
SettingsLayerEditor editor_hook(SettingsLayer*, u64 control) noexcept {
    if (selected_permissions() || !(control == 1 || (control == 2 && owner->text)))
        return {};
    return {&owner->models[control - 1], &owner->bindings[control - 1], control == 2};
}
bool edited_hook(SettingsLayer*, u64 control) noexcept {
    const auto text = message_composer_text(owner->models[control - 1]);
    if (owner->text) {
        ChannelOverviewPatch patch{};
        patch.fields = control == 2 ? channel_overview_topic : channel_overview_name;
        if (control == 2)
            patch.topic = text;
        else
            patch.name = text;
        return channel_overview_controller_edit(&owner->controller, patch);
    }
    // The model edits draft.name in place; only the length needs updating.
    owner->draft.name_bytes = static_cast<u16>(minimum(text.bytes, kGuildAdminNameBytes));
    return true;
}
void close_hook(SettingsLayer*) noexcept {
    request_close();
}

} // namespace channel_overview_detail
namespace {

void request_close() noexcept {
    if (!owner)
        return;
    settings_layer_close_choice(&owner->layer);
    if (guard_dirty()) {
        repaint();
        return;
    }
    application_channel_overview_close();
}

} // namespace
namespace channel_overview_detail {

// Rebaseline clean drafts from the stores; flag dirty ones.
bool reconcile() noexcept {
    auto& p = *owner;
    const u64 now = clock_monotonic_milliseconds();
    bool changed = false;
    if (!busy()) {
        const auto* channel = chat_state_store_find_channel(state(), p.identity.channel_id);
        const auto stored = channel ? chat_state_store_channel_overwrites(state(), *channel)
                                    : ConstSlice<PermissionOverwriteRecord>{};
        const bool same = permission_overwrite_draft_equal(p.overwrite_base, stored);
        if (p.overwrite_echo) {
            if (same || deadline_due(now, p.overwrite_echo_until))
                p.overwrite_echo = false;
        } else if (!same) {
            if (overwrites_dirty())
                changed = !p.overwrite_conflict, p.overwrite_conflict = true;
            else {
                load_overwrites();
                changed = true;
            }
        }
    }
    if (p.echo && deadline_due(now, p.echo_until))
        p.echo = false;
    return changed;
}

const HostedModal* active_modal() noexcept {
    for (const auto& modal : kModals)
        if (modal.active())
            return &modal;
    return nullptr;
}

} // namespace channel_overview_detail

bool application_channel_overview_active() noexcept {
    return active_modal() || (owner && owner->scene.active);
}

} // namespace maicord::app_detail
