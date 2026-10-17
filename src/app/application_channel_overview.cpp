#include "application_channel_overview.hpp"
#include "application_guild_settings.hpp"
#include "application_poll_create.hpp"
#include "application_guild_lifecycle.hpp"
#include "application_profile_editor.hpp"
#include "application_report.hpp"
#include "application_user_actions.hpp"
#include "application_guild_community.hpp"
#include "application_custom_status.hpp"
#include "application_settings_layer.hpp"
#include "maicord/render/layer_presentation.hpp"
#include "application_state.hpp"
#include "application_settings.hpp"
#include "application_account_presentation.hpp"
#include "application_account_module.hpp"
#include "application_composer_text_input.hpp"
#include "application_message_interaction.hpp"
#include "application_media_picker.hpp"
#include "application_profile.hpp"
#include "application_text_form.hpp"
#include "application_attachments.hpp"
#include "maicord/app/channel_overview_controller.hpp"
#include "maicord/model/channel_settings_model.hpp"
#include "maicord/model/guild_settings_model.hpp"
#include "maicord/model/permission_grid.hpp"
#include "maicord/model/bounded_edit.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/deadline.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
// Channel Settings: Overview (text channels through the overview controller,
// voice/stage/category through a channel PATCH), Permissions (overrides with
// allow / neutral / deny per permission) and Delete channel.
constexpr UiPageKey page_key{0x43484F5645525657ull, 1};
constexpr UiPageKey permissions_key{0x43484F5645525657ull, 2};
constexpr u64 kDeleteAction = 1;
constexpr u32 slowmode_values[]{0, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 21600};
constexpr Utf8View slowmode_labels[]{
    utf8_literal("Off"),        utf8_literal("5 seconds"),  utf8_literal("10 seconds"),
    utf8_literal("15 seconds"), utf8_literal("30 seconds"), utf8_literal("1 minute"),
    utf8_literal("2 minutes"),  utf8_literal("5 minutes"),  utf8_literal("10 minutes"),
    utf8_literal("15 minutes"), utf8_literal("30 minutes"), utf8_literal("1 hour"),
    utf8_literal("2 hours"),    utf8_literal("6 hours"),    utf8_literal("Current custom value")};
constexpr Utf8View kTriState[]{utf8_literal("Deny"), utf8_literal("Neutral"), utf8_literal("Allow")};
constexpr u32 kControlCapacity = 768;
constexpr u32 kBankBytes = 48 * 1024;
constexpr u32 kPopupCapacity = 320;
constexpr u32 kTargetBase = 1'000;
constexpr u64 kEchoMs = 8'000;
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
enum class Op : u8 {
    none,
    fetch,
    update,
    overwrites,
    remove
};
enum class Menu : u8 {
    none,
    slowmode,
    target
};
struct Overview {
    ChannelOverviewController controller;
    UiSettingsScene scene;
    SettingsLayer layer;
    UiPageRegistry registry;
    UiPage pages[2];
    UiSettingsPageContent content, permissions_content;
    UiSettingsNavEntry nav[6];
    u32 nav_count, nav_revision;
    UiSettingsControl controls[kControlCapacity];
    UiSettingsControlPresentation presentation[kControlCapacity];
    UiSettingsAvatarMedia avatar_media[kControlCapacity];
    u32 control_count;
    UiSettingsEditor editors[2];
    UiSettingsFooter footer;
    MessageComposer models[2];
    BoundedEdit bindings[2];
    UiWidgetManager* previous_focus;
    u64 lifetime, projected_revision;
    bool permission_observed;
    // Channel identity and capabilities.
    ChannelOverviewScope identity;
    u8 type;
    bool text, can_overview, can_permissions, can_delete;
    // Voice, stage and category overview.
    ChannelSettingsDraft base, draft;
    bool loaded, fetch_failed, conflict, echo;
    u64 echo_until;
    // Permission overrides.
    PermissionOverwriteDraft overwrite_base, overwrite_draft;
    u64 selected_target;
    bool overwrite_conflict, overwrite_echo;
    u64 overwrite_echo_until;
    // Transport for everything except the text overview.
    Op op;
    bool queued, failed;
    u64 request_id, retry_at;
    Utf8View status;
    Menu menu;
    u64 menu_ids[kPopupCapacity];
    u8 menu_types[kPopupCapacity];
    PopupItem popup_items[kPopupCapacity];
    char popup_text[24 * 1024];
    char header[kGuildAdminNameBytes + 4];
    char banks[2][kBankBytes];
    u32 bank_index;
    Utf8View user_limit_labels[kChannelSettingsUserLimitMaximum + 1];
    char user_limit_text[1'200];
    Utf8View bitrate_labels[16];
    // Packed RGBA accents of override rows; the last slot is the implicit @everyone.
    u32 row_accent[kPermissionOverwriteDraftMaximum + 1];
    u64 palette_revision;
    u32 bitrate_values[16];
    u32 bitrate_count;
    char bitrate_text[160];
};
Overview* owner;
u64 lifetime;

using Bank = TextBank;

u32 person_color(u64 user_id) noexcept {
    constexpr u32 colors[]{0x5865f2ff, 0x757e8aff, 0x3ba55cff, 0xfaa61aff, 0xed4245ff, 0xeb459fff};
    return colors[(user_id >> 22) % 6];
}
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
// Channel-level permissions of the current account.
PermissionSet self_permissions(u64 channel_id) noexcept {
    const auto* channel = chat_state_store_find_channel(state(), channel_id);
    if (!channel || !channel->guild_id)
        return 0;
    u64 roles[kGuildRoleMaximum];
    u32 count = 0;
    for (u32 i = 0; i < state().role_count && count < kGuildRoleMaximum; ++i)
        if (state().roles[i].guild_id == channel->guild_id && state().roles[i].id != channel->guild_id &&
            state().roles[i].self_assigned)
            roles[count++] = state().roles[i].id;
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
// Text overview through the overview controller (unchanged contract).
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
bool editable() noexcept {
    if (!owner || !owner->scene.active || owner->layer.choice || g_app.text_form || busy())
        return false;
    if (owner->text && !selected_permissions())
        return owner->controller.active && owner->controller.available && owner->controller.can_manage &&
               owner->controller.authoritative && !owner->controller.request_id && !owner->controller.queued;
    return (channel_settings_supported(owner->type) || selected_permissions()) &&
           scope(owner->identity.channel_id).channel_id != 0;
}
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
    MessageComposer* focused = nullptr;
    return editable() && owner->lifetime == identity &&
           settings_layer_editor(&owner->layer, nullptr, &focused, nullptr, nullptr) && focused == model &&
           channel_overview_scope_equal(scope(owner->identity.channel_id), owner->identity);
}
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
        return utf8_literal("You have unsaved changes.");
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
    return c.dirty_fields ? utf8_literal("You have unsaved changes.") : Utf8View{};
}

// Projection ------------------------------------------------------------

u32 push(const UiSettingsControl& control, UiSettingsControlPresentation presentation = {}) noexcept {
    if (owner->control_count >= kControlCapacity)
        return ~0u;
    owner->controls[owner->control_count] = control;
    owner->presentation[owner->control_count] = presentation;
    owner->avatar_media[owner->control_count] = {};
    return owner->control_count++;
}
UiSettingsEditor* editor(u32 index, u64 control, Utf8View placeholder, bool enabled) noexcept {
    owner->editors[index] =
        settings_layer_editor_view(&owner->layer, control, owner->models[index], placeholder, enabled);
    return &owner->editors[index];
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
    p.controls[0] = {1, utf8_literal("Channel name"), {}, UiSettingsControlKind::edit, edit, false, {}, 0, {},
                     editor(0, 1, utf8_literal("Channel name"), edit)};
    p.controls[1] = {2, utf8_literal("Channel topic"), {}, UiSettingsControlKind::multiline, edit, false, {}, 0, {},
                     editor(1, 2, utf8_literal("Tell members what this channel is for"), edit)};
    p.controls[2] = {
        3,
        utf8_literal("Slowmode"),
        utf8_literal("Members can send one message per interval unless they can bypass slowmode."),
        UiSettingsControlKind::choice,
        edit,
        false,
        {slowmode_labels, 15},
        slowmode_index(p.controller.draft.slowmode_seconds),
        {}};
    p.controls[3] = {4,
                     utf8_literal("Age-restricted channel"),
                     utf8_literal("Members must confirm they meet the age requirement to view this channel."),
                     UiSettingsControlKind::toggle,
                     edit,
                     p.controller.draft.nsfw,
                     {},
                     0,
                     {}};
    const bool conflict = p.controller.phase == ChannelOverviewPhase::conflict;
    p.controls[4] = {
        5,
        conflict ? utf8_literal("Review your changes") : utf8_literal("Refresh channel settings"),
        conflict ? utf8_literal("Keep your changes on top of the latest settings.")
                 : utf8_literal("Reload the latest settings. Unsaved changes are kept."),
        UiSettingsControlKind::action,
        p.controller.available && p.controller.can_manage && !p.controller.request_id && !p.controller.queued,
        false,
        {},
        0,
        conflict ? utf8_literal("Use my changes") : utf8_literal("Refresh")};
    for (u32 i = 0; i < 5; ++i)
        p.presentation[i] = {};
    if (conflict)
        p.presentation[4].variant = UiSettingsVariant::primary;
    p.control_count = 5;
}
void project_generic_overview(Bank& bank) noexcept {
    auto& p = *owner;
    const bool edit = editable() && p.loaded && p.can_overview;
    auto& d = p.draft;
    if (p.fetch_failed)
        push({control_notice, utf8_literal("Couldn't load this channel"), p.status, UiSettingsControlKind::notice,
              true, false, {}, 0, utf8_literal("Try again"), nullptr},
             {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    else if (!p.loaded)
        push({control_notice, {}, utf8_literal("Loading…"), UiSettingsControlKind::notice, true, false, {}, 0, {},
              nullptr});
    else if (p.conflict)
        push({control_notice, utf8_literal("Changed elsewhere"),
              utf8_literal("Someone else updated this channel. Load the latest values or keep editing."),
              UiSettingsControlKind::notice, true, false, {}, 0, utf8_literal("Load latest"), nullptr},
             {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    push({1, p.type == 4 ? utf8_literal("Category name") : utf8_literal("Channel name"), {},
          UiSettingsControlKind::edit, edit, false, {}, 0, {},
          editor(0, 1, p.type == 4 ? utf8_literal("Category name") : utf8_literal("Channel name"), edit)});
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
    push({control_bitrate, utf8_literal("Bitrate (kbps)"),
          utf8_literal("Higher bitrates sound better but use more bandwidth."), UiSettingsControlKind::slider, edit,
          false, {p.bitrate_labels, p.bitrate_count}, selected, {}, nullptr},
         {UiSettingsVariant::standard, ui_settings_presentation_marks, 5});
    if (p.type == 2)
        push({control_user_limit, utf8_literal("User limit"),
              utf8_literal("How many members can join at once. Members who can move others ignore the limit."),
              UiSettingsControlKind::slider, edit, false, {p.user_limit_labels, kChannelSettingsUserLimitMaximum + 1},
              minimum(d.user_limit, kChannelSettingsUserLimitMaximum), {}, nullptr});
    push({3, utf8_literal("Slowmode"),
          utf8_literal("Members can send one message per interval unless they can bypass slowmode."),
          UiSettingsControlKind::choice, edit, false, {slowmode_labels, 15}, slowmode_index(d.slowmode_seconds), {},
          nullptr});
    push({4, utf8_literal("Age-restricted channel"),
          utf8_literal("Members must confirm they meet the age requirement to view this channel."),
          UiSettingsControlKind::toggle, edit, d.nsfw, {}, 0, {}, nullptr});
    (void)bank;
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
    bank.add(utf8_literal("Member "));
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
    heap_sort(order, count, [&](u32 a, u32 b) noexcept {
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
        push({control_notice, {}, p.status, UiSettingsControlKind::notice, true, false, {}, 0, {}, nullptr},
             {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    if (p.overwrite_conflict)
        push({control_notice, utf8_literal("Changed elsewhere"),
              utf8_literal("Someone else changed these permissions. Load the latest values or keep editing."),
              UiSettingsControlKind::notice, true, false, {}, 0, utf8_literal("Load latest"), nullptr},
             {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    if (const auto* parent = parent_channel()) {
        const bool synced = permission_overwrite_draft_equal(p.overwrite_draft,
                                                             chat_state_store_channel_overwrites(state(), *parent));
        const u32 start = bank.used;
        bank.add(synced ? utf8_literal("Synced with ") : utf8_literal("Not synced with "));
        bank.add(chat_state_store_channel_name(state(), *parent));
        push({control_sync, Utf8View{bank.data + start, bank.used - start},
              synced ? utf8_literal("This channel uses its category's permissions.")
                     : utf8_literal("Sync to copy the category's permissions to this channel."),
              UiSettingsControlKind::notice, edit || synced, false, {}, 0,
              synced ? Utf8View{} : utf8_literal("Sync now"), nullptr},
             {synced ? UiSettingsVariant::positive : UiSettingsVariant::standard, 0, kUiSettingsNoDefault});
    }
    push({control_targets, utf8_literal("Roles and members"),
          utf8_literal("Overrides replace role permissions for this channel. Pick one to edit it."),
          UiSettingsControlKind::category, true, false, {}, 0, {}, nullptr});
    push({control_add_target, utf8_literal("Add a role or member"), {}, UiSettingsControlKind::action, edit, false,
          {}, 0, utf8_literal("Add"), nullptr});
    // @everyone is always listed, even without an explicit override.
    if (permission_overwrite_draft_find(p.overwrite_draft, p.identity.guild_id) == ~0u) {
        push({kTargetBase + kPermissionOverwriteDraftMaximum, utf8_literal("@everyone"), utf8_literal("Role"),
              UiSettingsControlKind::action, true, false, {}, 0, {}, nullptr},
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
        u32 overrides = 0;
        for (u64 bits = entry.allow | entry.deny; bits; bits &= bits - 1)
            ++overrides;
        const u32 start = bank.used;
        bank.add(member ? utf8_literal("Member") : utf8_literal("Role"));
        if (overrides) {
            bank.add(utf8_literal(" · "));
            bank.number(overrides);
            bank.add(overrides == 1 ? utf8_literal(" override") : utf8_literal(" overrides"));
        }
        u8 flags = ui_settings_presentation_compact;
        if (entry.id == p.selected_target)
            flags |= ui_settings_presentation_selected;
        if (member)
            flags |= ui_settings_presentation_avatar;
        const u32 target = push({kTargetBase + order[i], name, Utf8View{bank.data + start, bank.used - start},
                                 UiSettingsControlKind::action, true, false, {}, 0,
                                 entry.id == p.identity.guild_id || !edit ? Utf8View{} : utf8_literal("Remove"),
                                 nullptr},
                                {UiSettingsVariant::standard, static_cast<u8>(flags | ui_settings_presentation_accent),
                                 kUiSettingsNoDefault});
        // Member targets draw the cached avatar over their initials once resident.
        if (member && target < kControlCapacity)
            p.avatar_media[target] = application_user_avatar_media(entry.id);
        p.row_accent[order[i]] = member ? person_color(entry.id) : (color ? color : guild_role_color_rgba(0));
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
    bank.add(utf8_literal("Permissions for "));
    bank.add(name);
    push({control_grid, Utf8View{bank.data + start, bank.used - start},
          utf8_literal("Deny, keep neutral to inherit, or allow each permission."), UiSettingsControlKind::category,
          true, false, {}, 0, {}, nullptr});
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
                push({control_group + group, permission_group_label(static_cast<PermissionGroup>(group)), {},
                      UiSettingsControlKind::category, true, false, {}, 0, {}, nullptr});
                first = false;
            }
            const auto value = permission_tri_get(allow, deny, e.bit);
            push({control_permission + i, permission_grid_label(e, scope_bits),
                  permission_grid_description(e, scope_bits), UiSettingsControlKind::segments,
                  edit && ((grantable & e.bit) == e.bit || value != PermissionTriState::neutral), false,
                  {kTriState, 3}, static_cast<u32>(value), {}, nullptr},
                 {UiSettingsVariant::standard, ui_settings_presentation_tristate, kUiSettingsNoDefault});
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
    *theme = {};
    theme->colors[0] = rgba >> 8;
    theme->color_count = 1;
    return true;
}
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
        project_generic_overview(bank);
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
    other.controls = {};
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
    if (!project())
        return false;
    application_request_frame();
    return !g_app.renderer_ready || renderer_rebuild_content_text(&g_app.renderer, application_theme());
}
void build_navigation() noexcept {
    auto& p = *owner;
    const auto* channel = chat_state_store_find_channel(state(), p.identity.channel_id);
    Bank header{p.header, sizeof(p.header), 0};
    if (channel) {
        if (p.type == 0 || p.type == 5 || p.type == 15)
            header.add(utf8_literal("#"));
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

// Drafts and requests -----------------------------------------------------

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
    owner->status = {};
    application_request_frame();
}
bool guard_dirty() noexcept {
    if (!page_dirty())
        return false;
    settings_layer_shake(&owner->layer, clock_monotonic_milliseconds());
    return true;
}
bool form_valid(TextFormOwnerKey key) noexcept {
    return owner && owner->scene.active && key.scope == owner->lifetime && key.item == owner->identity.channel_id &&
           channel_overview_scope_equal(scope(key.item), owner->identity);
}
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
    bank.add(utf8_literal("Delete "));
    if (owner->type == 0 || owner->type == 5 || owner->type == 15)
        bank.add(utf8_literal("#"));
    bank.add(chat_state_store_channel_name(state(), *channel));
    bank.add(utf8_literal("?"));
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
void open_slowmode(UiRect anchor, u32 current) noexcept {
    for (u32 i = 0; i < 14; ++i)
        owner->popup_items[i] = {slowmode_labels[i], 700 + i,
                                 static_cast<u16>(popup_item_enabled | (slowmode_values[i] == current ? popup_item_checked : 0))};
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
        if (permission_overwrite_draft_find(p.overwrite_draft, roles[i]->id) != ~0u)
            continue;
        const bool everyone = roles[i]->id == p.identity.guild_id;
        if (everyone)
            continue;
        p.popup_items[count] = {};
        p.popup_items[count].label = bank.add(chat_state_store_role_name(state(), *roles[i]));
        p.popup_items[count].detail = utf8_literal("Role");
        p.popup_items[count].action = 2'000 + count;
        p.popup_items[count].flags = popup_item_enabled;
        p.menu_ids[count] = roles[i]->id;
        p.menu_types[count++] = 0;
    }
    const auto& members = g_app.account_ui->guild_member_store;
    const auto& users = g_app.account_ui->user_store;
    bool separated = false;
    for (u32 i = 0; i < members.capacity && count < kPopupCapacity; ++i) {
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
        p.popup_items[count] = {};
        p.popup_items[count].label = bank.add(name);
        p.popup_items[count].detail = utf8_literal("Member");
        p.popup_items[count].action = 2'000 + count;
        p.popup_items[count].flags =
            static_cast<u16>(popup_item_enabled | (!separated && count ? popup_item_separator_before : 0));
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

// Activation ----------------------------------------------------------------

void request_close() noexcept;
void select(UiPageKey key) noexcept {
    auto& p = *owner;
    if (guard_dirty())
        return;
    settings_layer_close_choice(&p.layer);
    p.failed = false;
    p.status = {};
    if (ui_page_key_equal(key, permissions_key))
        load_overwrites();
    ui_page_registry_select(&p.registry, key);
    project();
    ui_settings_scene_focus(&p.scene, {UiSettingsTargetKind::page, key, 0});
    repaint();
}
void activate_hook(SettingsLayer*, const SettingsLayerActivation& activation) noexcept {
    if (!owner)
        return;
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
        p.status = {};
        if (permissions)
            load_overwrites();
        else if (p.text) {
            if (channel_overview_controller_reset(&c))
                import_models();
        } else {
            p.draft = p.base;
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
    if (!owner)
        return;
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
bool editor_hook(SettingsLayer*, u64 control, MessageComposer** model, BoundedEdit** binding, bool* multiline) noexcept {
    if (!owner || selected_permissions() || !(control == 1 || (control == 2 && owner->text)))
        return false;
    *model = &owner->models[control - 1];
    *binding = &owner->bindings[control - 1];
    *multiline = control == 2;
    return true;
}
bool edited_hook(SettingsLayer*, u64 control) noexcept {
    if (!owner)
        return false;
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
bool repaint_hook(SettingsLayer*) noexcept {
    return repaint();
}
bool editable_hook(SettingsLayer*) noexcept {
    return editable();
}
constexpr SettingsLayerHooks kHooks{editor_hook, edited_hook, activate_hook, choose_hook, close_hook, repaint_hook,
                                    nullptr, editable_hook};

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
} // namespace

bool application_channel_overview_active() noexcept {
    return application_guild_settings_active() || application_poll_create_active() ||
           application_guild_lifecycle_active() || application_guild_community_active() ||
           application_profile_editor_active() || application_report_active() ||
           application_custom_status_active() || (owner && owner->scene.active);
}
bool application_channel_overview_open(u64 channel_id) noexcept {
    if (!available() || !g_app.renderer_ready || g_app.text_form || application_channel_overview_active() ||
        (owner && (owner->controller.request_id || owner->request_id)) || lifetime == ~u64{0} ||
        !(manage(channel_id) || manage_channel(channel_id) || manage_overrides(channel_id)))
        return false;
    application_settings_close(false);
    application_media_picker_close();
    application_profile_close();
    if (!owner) {
        owner = static_cast<Overview*>(
            VirtualAlloc(nullptr, sizeof(Overview), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!owner)
            return false;
        owner->layer.scene = &owner->scene;
        owner->layer.hooks = &kHooks;
        owner->layer.owner = owner;
        // User limit labels: "No limit", then the member count.
        Bank text{owner->user_limit_text, sizeof(owner->user_limit_text), 0};
        owner->user_limit_labels[0] = utf8_literal("No limit");
        for (u32 i = 1; i <= kChannelSettingsUserLimitMaximum; ++i) {
            const u32 start = text.used;
            text.number(i);
            text.add(i == 1 ? utf8_literal(" user") : utf8_literal(" users"));
            owner->user_limit_labels[i] = {owner->user_limit_text + start, text.used - start};
        }
    }
    auto& p = *owner;
    const auto* channel = chat_state_store_find_channel(state(), channel_id);
    p.lifetime = ++lifetime;
    p.previous_focus = g_app.context_focus;
    p.permission_observed = false;
    p.identity = scope(channel_id);
    p.type = channel->type;
    p.text = channel->type == 0 && manage(channel_id);
    p.can_overview = p.text || (channel_settings_supported(p.type) && manage_channel(channel_id));
    p.can_permissions = manage_overrides(channel_id);
    p.can_delete = manage_channel(channel_id);
    p.loaded = p.fetch_failed = p.conflict = p.echo = false;
    p.op = Op::none;
    p.queued = p.failed = false;
    p.request_id = 0;
    p.status = {};
    p.menu = Menu::none;
    p.selected_target = channel->guild_id;
    settings_layer_reset(&p.layer);
    for (auto& model : p.models)
        message_composer_release(&model);
    if (p.text) {
        if (!channel_overview_controller_open(&p.controller, p.identity, true, true))
            return false;
        message_composer_initialize(&p.models[0], {p.controller.draft.name, kChannelOverviewNameBytes});
        message_composer_initialize(&p.models[1], {p.controller.draft.topic, kChannelOverviewTopicBytes});
    } else {
        p.controller = {};
        p.base = p.draft = {};
        p.draft.channel_id = channel_id;
        message_composer_initialize(&p.models[0], {p.draft.name, kGuildAdminNameBytes});
    }
    load_overwrites();
    if (!import_models()) {
        application_channel_overview_close(false);
        return false;
    }
    p.content = {utf8_literal("Overview"), Utf8View{}, {}, 1, kUiSettingsPageFormat, &kUiDefaultSettingsStyle,
                 &p.footer};
    p.permissions_content = {utf8_literal("Permissions"), Utf8View{}, {}, 1, kUiSettingsPageFormat,
                             &kUiDefaultSettingsStyle, nullptr};
    if (!ui_page_registry_initialize(&p.registry, {p.pages, 2}) ||
        !ui_page_registry_register(&p.registry, {page_key, utf8_literal("Overview"), &p.content}).ok() ||
        !ui_page_registry_register(&p.registry, {permissions_key, utf8_literal("Permissions"), &p.permissions_content})
             .ok()) {
        application_channel_overview_close(false);
        return false;
    }
    ui_page_registry_select(&p.registry, p.can_overview ? page_key : permissions_key);
    if (!p.text && p.can_overview)
        queue(Op::fetch);
    application_composer_text_input_suspend();
    dismiss_popup();
    ui_scroll_host_cancel(&g_app.scroll_host);
    g_app.message_composer.focused = false;
    ui_settings_scene_set_presentation(&p.scene, UiSettingsPresentation::layer);
    build_navigation();
    // The first update activates the scene; project() fills the page after.
    if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()) || !project()) {
        application_channel_overview_close(false);
        return false;
    }
    renderer_bind_settings_scene(&g_app.renderer, &p.scene);
    settings_layer_begin_entrance(&p.layer, clock_monotonic_milliseconds(), g_app.preferences.reduced_motion);
    g_app.context_focus = &p.scene.widgets;
    application_sync_attachment_drop_target();
    synchronize_member_subscription(clock_monotonic_milliseconds());
    if (repaint())
        return true;
    application_channel_overview_close(false);
    return false;
}
void application_channel_overview_close(bool restore_focus) noexcept {
    if (application_guild_settings_active()) {
        application_guild_settings_close(restore_focus);
        return;
    }
    if (application_poll_create_active()) {
        application_poll_create_close(restore_focus);
        return;
    }
    if (application_guild_lifecycle_active()) {
        application_guild_lifecycle_close(restore_focus);
        return;
    }
    if (application_profile_editor_active()) {
        application_profile_editor_close(restore_focus);
        return;
    }
    if (application_guild_community_active()) {
        application_guild_community_close(restore_focus);
        return;
    }
    if (application_report_active()) {
        application_report_close(restore_focus);
        return;
    }
    if (application_custom_status_active()) {
        application_custom_status_close(restore_focus);
        return;
    }
    if (!owner || (!owner->scene.active && !owner->controller.active))
        return;
    application_composer_text_input_suspend();
    settings_layer_close_choice(&owner->layer);
    if (owner->scene.active)
        settings_layer_cancel_pointer(&owner->layer);
    application_text_form_cancel_owner(form_valid);
    channel_overview_controller_cancel(&owner->controller);
    owner->queued = false;
    renderer_bind_settings_scene(&g_app.renderer, nullptr);
    owner->layer.entrance_started = 0;
    renderer_set_settings_modal_progress(&g_app.renderer, 1);
    ui_settings_scene_close(&owner->scene);
    owner->layer.blink_at = owner->layer.shake_started = 0;
    owner->permission_observed = false;
    if (restore_focus && available())
        g_app.context_focus = &g_app.shell_scene.widgets;
    application_sync_attachment_drop_target();
    synchronize_member_subscription(clock_monotonic_milliseconds());
    if (g_app.renderer_ready)
        refresh_account_text(true);
    application_request_frame();
}
void application_channel_overview_release() noexcept {
    application_guild_settings_release();
    application_poll_create_release();
    application_guild_lifecycle_release();
    application_profile_editor_release();
    application_guild_community_release();
    application_report_release();
    application_user_actions_release();
    application_custom_status_release();
    application_channel_overview_close(false);
    if (!owner)
        return;
    ui_settings_scene_release(&owner->scene);
    for (auto& model : owner->models)
        message_composer_release(&model);
    SecureZeroMemory(owner, sizeof(*owner));
    VirtualFree(owner, 0, MEM_RELEASE);
    owner = nullptr;
}
bool application_channel_overview_synchronize() noexcept {
    if (application_guild_settings_active())
        return application_guild_settings_synchronize();
    if (application_poll_create_active())
        return application_poll_create_synchronize();
    if (application_guild_lifecycle_active())
        return application_guild_lifecycle_synchronize();
    if (application_profile_editor_active())
        return application_profile_editor_synchronize();
    if (application_guild_community_active())
        return application_guild_community_synchronize();
    if (application_report_active())
        return application_report_synchronize();
    if (application_custom_status_active())
        return application_custom_status_synchronize();
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    auto& c = p.controller;
    const auto current = scope(p.identity.channel_id);
    if (!channel_overview_scope_equal(current, p.identity) || !g_app.account_ui || g_app.login.visible ||
        !(manage(current.channel_id) || manage_channel(current.channel_id) || manage_overrides(current.channel_id) ||
          (p.text && c.active))) {
        application_text_form_cancel_owner(form_valid);
        application_channel_overview_close(false);
        return true;
    }
    bool changed = false;
    const bool permissions = manage_overrides(current.channel_id), deletable = manage_channel(current.channel_id);
    if (permissions != p.can_permissions || deletable != p.can_delete) {
        p.can_permissions = permissions;
        p.can_delete = deletable;
        build_navigation();
        changed = true;
    }
    if (p.text) {
        const auto phase = c.phase;
        const bool online = c.available, permission = c.can_manage;
        if (p.permission_observed) {
            p.permission_observed = false;
            channel_overview_controller_reauthorize(&c, current, available(), manage(current.channel_id));
        } else
            channel_overview_controller_authority(&c, current, available(), manage(current.channel_id));
        changed = changed || phase != c.phase || online != c.available || permission != c.can_manage ||
                  p.projected_revision != c.revision;
    }
    if (p.layer.choice && !settings_layer_choice_current(p.layer)) {
        p.layer.choice = false;
        p.menu = Menu::none;
        changed = true;
    }
    changed = reconcile() || changed;
    if (changed) {
        if (!editable())
            application_composer_text_input_suspend();
        return repaint();
    }
    if (!ui_settings_scene_update(&p.scene, &p.registry, application_viewport_dips()))
        return false;
    return g_app.renderer.interaction_text_settings_revision == p.scene.revision ||
           renderer_rebuild_content_text(&g_app.renderer, application_theme());
}
bool application_channel_overview_submit(u64 now) noexcept {
    if (application_guild_settings_next_wake())
        return application_guild_settings_submit(now);
    if (application_poll_create_next_wake())
        return application_poll_create_submit(now);
    if (application_guild_lifecycle_next_wake())
        return application_guild_lifecycle_submit(now);
    if (application_profile_editor_next_wake())
        return application_profile_editor_submit(now);
    if (application_guild_community_next_wake())
        return application_guild_community_submit(now);
    if (application_report_next_wake())
        return application_report_submit(now);
    if (const u64 social = application_user_actions_next_wake(); social && social <= now &&
                                                                application_user_actions_submit(now))
        return true;
    if (!owner || g_app.details_request_owner != MessageHistoryRequestOwner::none ||
        !application_channel_overview_synchronize() || !owner->scene.active)
        return false;
    auto& p = *owner;
    if (p.text) {
        const auto request = channel_overview_controller_request(p.controller, now);
        if (request.ready && g_app.account.api.channel_overview && g_app.account.instance) {
            u64 id = ++g_app.account_ui->message_history_request_id;
            if (!id)
                id = ++g_app.account_ui->message_history_request_id;
            const auto submitted = g_app.account.api.channel_overview(
                g_app.account.instance, request.operation, request.scope.channel_id,
                request.operation == ChannelOverviewOperation::update ? &request.patch : nullptr, id);
            if (!submitted.ok()) {
                if (submitted.error == RestClientSubmitError::busy)
                    p.controller.retry_at_ms = deadline_after(now, 50);
                else {
                    channel_overview_controller_admission_failed(&p.controller, request,
                                                                 ChannelOverviewFailure::rejected);
                    repaint();
                }
                return true;
            }
            if (!channel_overview_controller_submitted(&p.controller, request, id))
                return false;
            g_app.details_request_owner = MessageHistoryRequestOwner::channel_overview;
            g_app.details_request_id = id;
            repaint();
            return true;
        }
    }
    if (!p.queued || p.request_id || (p.retry_at && p.retry_at > now) || !g_app.account.instance)
        return false;
    u64 id = ++g_app.account_ui->message_history_request_id;
    if (!id)
        id = ++g_app.account_ui->message_history_request_id;
    RestClientSubmitResult submitted{RestClientSubmitError::invalid_input};
    // Channel routes are channel-major: no guild ID.
    GuildAdminRequest request{};
    request.channel_id = p.identity.channel_id;
    ChannelPatch patch{};
    PermissionOverwriteWrite overwrites[kPermissionOverwriteDraftMaximum];
    bool valid = true;
    if (p.op == Op::fetch) {
        if (g_app.account.api.channel_overview)
            submitted = g_app.account.api.channel_overview(g_app.account.instance, ChannelOverviewOperation::fetch,
                                                           p.identity.channel_id, nullptr, id);
    } else {
        if (p.op == Op::update) {
            request.operation = GuildAdminOperation::channel_update;
            valid = channel_settings_patch(p.base, p.draft, &patch);
            request.channel = &patch;
        } else if (p.op == Op::overwrites) {
            request.operation = GuildAdminOperation::channel_update;
            const u32 count = permission_overwrite_draft_write(p.overwrite_draft, {overwrites, kPermissionOverwriteDraftMaximum});
            valid = count != ~0u;
            patch.fields = channel_patch_overwrites;
            patch.overwrites = {overwrites, valid ? count : 0};
            request.channel = &patch;
        } else
            request.operation = GuildAdminOperation::channel_delete;
        if (valid && g_app.account.api.guild_admin)
            submitted = g_app.account.api.guild_admin(g_app.account.instance, &request, id);
    }
    if (submitted.error == RestClientSubmitError::busy) {
        p.retry_at = deadline_after(now, 50);
        return false;
    }
    if (!submitted.ok()) {
        p.queued = false;
        p.failed = true;
        p.status = utf8_literal("This change couldn't be sent.");
        if (p.op == Op::fetch)
            p.fetch_failed = true;
        repaint();
        return false;
    }
    p.queued = false;
    p.request_id = id;
    g_app.details_request_owner = MessageHistoryRequestOwner::channel_overview;
    g_app.details_request_id = id;
    repaint();
    return true;
}
bool application_channel_overview_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (application_guild_settings_owns_response(response.request_id))
        return application_guild_settings_response(response, body);
    if (application_poll_create_owns_response(response.request_id))
        return application_poll_create_response(response, body);
    if (application_guild_lifecycle_owns_response(response.request_id))
        return application_guild_lifecycle_response(response, body);
    if (application_profile_editor_owns_response(response.request_id))
        return application_profile_editor_response(response, body);
    if (application_guild_community_owns_response(response.request_id))
        return application_guild_community_response(response, body);
    if (application_report_owns_response(response.request_id))
        return application_report_response(response, body);
    if (application_user_actions_owns_response(response.request_id))
        return application_user_actions_response(response, body);
    if (!owner)
        return false;
    auto& p = *owner;
    if (p.request_id && p.request_id == response.request_id) {
        p.request_id = 0;
        const bool ok = response.error == RestClientError::none && response.status_code >= 200 &&
                        response.status_code < 300;
        if (!p.scene.active || !channel_overview_scope_equal(scope(p.identity.channel_id), p.identity))
            return true;
        const u64 now = clock_monotonic_milliseconds();
        if (!ok) {
            p.failed = true;
            p.status = response.error != RestClientError::none ? utf8_literal("Couldn't reach Discord. Try again.")
                       : response.status_code == 403 ? utf8_literal("You don't have permission to do this.")
                       : response.status_code == 429 ? utf8_literal("Too many changes at once. Try again shortly.")
                                                     : utf8_literal("Discord rejected this change.");
            if (p.op == Op::fetch)
                p.fetch_failed = true;
            return repaint();
        }
        if (p.op == Op::fetch) {
            ChannelSettingsDraft fetched{};
            if (channel_settings_decode(body, &fetched) && fetched.channel_id == p.identity.channel_id) {
                if (p.loaded && !channel_settings_equal(p.base, p.draft) && !channel_settings_equal(fetched, p.base))
                    p.conflict = true;
                else {
                    p.base = p.draft = fetched;
                    p.loaded = true;
                    p.conflict = false;
                    import_models();
                }
            } else {
                p.fetch_failed = true;
                p.status = utf8_literal("The channel details couldn't be read.");
            }
        } else if (p.op == Op::update) {
            p.base = p.draft;
            p.echo = true;
            p.echo_until = deadline_after(now, kEchoMs);
        } else if (p.op == Op::overwrites) {
            p.overwrite_base = p.overwrite_draft;
            p.overwrite_echo = true;
            p.overwrite_echo_until = deadline_after(now, kEchoMs);
        } else if (p.op == Op::remove) {
            application_channel_overview_close();
            return true;
        }
        return repaint();
    }
    if (p.controller.request_id != response.request_id)
        return false;
    if (!application_channel_overview_synchronize())
        return false;
    ChannelOverviewCompletion completion{ChannelOverviewCompletionKind::ambiguous, 0};
    ChannelOverviewSnapshot remote{};
    bool decoded = false;
    if (response.transport_ok()) {
        if (response.status_code == 403 || response.status_code == 404)
            completion.kind = ChannelOverviewCompletionKind::permission_lost;
        else if (response.status_code == 429) {
            completion.kind = ChannelOverviewCompletionKind::rate_limited;
            completion.retry_at_ms = response.retry_at_ms;
        } else if (response.status_code >= 400 && response.status_code < 500)
            completion.kind = ChannelOverviewCompletionKind::rejected;
        else if (response.status_code == 200) {
            completion.kind = ChannelOverviewCompletionKind::success;
            decoded = channel_overview_decode(body, false, nullptr, &remote).ok();
        }
    }
    if (!channel_overview_controller_complete(&p.controller, p.controller.scope, response.request_id, completion,
                                              decoded ? &remote : nullptr, clock_monotonic_milliseconds()))
        return false;
    if (!p.controller.active)
        return true;
    return import_models() && repaint();
}
void application_channel_overview_gateway(const GatewayClientEvent& event, Utf8View body) noexcept {
    application_guild_settings_gateway(event, body);
    if (!owner || !owner->controller.active)
        return;
    if (event.kind == GatewayClientEventKind::ready) {
        application_channel_overview_close(false);
        return;
    }
    if (event.kind != GatewayClientEventKind::dispatch)
        return;
    // The mailbox contains pointer-free model events, not raw channel JSON.
    // Refetch omitted metadata and invalidate an older outstanding REST request.
    if (event.dispatch_kind == GatewayClientDispatchKind::channel_update) {
        if (event.state.id == owner->controller.scope.channel_id &&
            event.state.guild_id == owner->controller.scope.guild_id) {
            channel_overview_controller_invalidate(&owner->controller, owner->controller.scope);
            if (event.state.fields & gateway_client_state_field_permission_overwrites)
                owner->permission_observed = true;
        }
    }
    const bool role_permissions = (event.dispatch_kind == GatewayClientDispatchKind::guild_role_create ||
                                   event.dispatch_kind == GatewayClientDispatchKind::guild_role_update) &&
                                  (event.state.fields & gateway_client_state_field_permissions);
    const bool permission_event = role_permissions ||
                                  event.dispatch_kind == GatewayClientDispatchKind::guild_role_delete ||
                                  (event.dispatch_kind == GatewayClientDispatchKind::guild_member_update &&
                                   event.state.id == owner->controller.scope.self_user_id);
    if (permission_event && event.state.guild_id == owner->controller.scope.guild_id)
        owner->permission_observed = true;
}
bool application_channel_overview_edit_focused() noexcept {
    if (application_guild_settings_active())
        return application_guild_settings_edit_focused();
    if (application_poll_create_active())
        return application_poll_create_edit_focused();
    if (application_guild_lifecycle_active())
        return application_guild_lifecycle_edit_focused();
    if (application_profile_editor_active())
        return application_profile_editor_edit_focused();
    if (application_guild_community_active())
        return application_guild_community_edit_focused();
    if (application_report_active())
        return false;
    if (application_custom_status_active())
        return application_custom_status_edit_focused();
    return owner && owner->scene.active && settings_layer_editor(&owner->layer, nullptr, nullptr, nullptr, nullptr);
}
MessageComposer* application_channel_overview_edit_model() noexcept {
    if (application_guild_settings_active())
        return application_guild_settings_edit_model();
    if (application_poll_create_active())
        return application_poll_create_edit_model();
    if (application_guild_lifecycle_active())
        return application_guild_lifecycle_edit_model();
    if (application_profile_editor_active())
        return application_profile_editor_edit_model();
    if (application_guild_community_active())
        return application_guild_community_edit_model();
    if (application_report_active())
        return nullptr;
    if (application_custom_status_active())
        return application_custom_status_edit_model();
    MessageComposer* model = nullptr;
    return owner && owner->scene.active && settings_layer_editor(&owner->layer, nullptr, &model, nullptr, nullptr)
               ? model
               : nullptr;
}
TextInputTarget application_channel_overview_edit_target() noexcept {
    if (application_guild_settings_active())
        return application_guild_settings_edit_target();
    if (application_poll_create_active())
        return application_poll_create_edit_target();
    if (application_guild_lifecycle_active())
        return application_guild_lifecycle_edit_target();
    if (application_profile_editor_active())
        return application_profile_editor_edit_target();
    if (application_guild_community_active())
        return application_guild_community_edit_target();
    if (application_report_active())
        return TextInputTarget{};
    if (application_custom_status_active())
        return application_custom_status_edit_target();
    BoundedEdit* binding = nullptr;
    return owner && owner->scene.active && settings_layer_editor(&owner->layer, nullptr, nullptr, &binding, nullptr)
               ? bounded_edit_input_target(binding)
               : TextInputTarget{};
}
void application_channel_overview_edit_cancel_native_group(TextInputTarget target) noexcept {
    application_guild_settings_edit_cancel_native_group(target);
    application_poll_create_edit_cancel_native_group(target);
    application_guild_lifecycle_edit_cancel_native_group(target);
    application_profile_editor_edit_cancel_native_group(target);
    application_guild_community_edit_cancel_native_group(target);
    application_custom_status_edit_cancel_native_group(target);
    if (owner && owner->scene.active)
        settings_layer_cancel_native_group(&owner->layer, target);
}
UiRect application_channel_overview_edit_bounds() noexcept {
    if (application_guild_settings_active())
        return application_guild_settings_edit_bounds();
    if (application_poll_create_active())
        return application_poll_create_edit_bounds();
    if (application_guild_lifecycle_active())
        return application_guild_lifecycle_edit_bounds();
    if (application_profile_editor_active())
        return application_profile_editor_edit_bounds();
    if (application_guild_community_active())
        return application_guild_community_edit_bounds();
    if (application_report_active())
        return UiRect{};
    if (application_custom_status_active())
        return application_custom_status_edit_bounds();
    return owner && owner->scene.active ? settings_layer_edit_bounds(&owner->layer) : UiRect{};
}
bool application_channel_overview_edit_changed(bool content) noexcept {
    if (application_guild_settings_active())
        return application_guild_settings_edit_changed(content);
    if (application_poll_create_active())
        return application_poll_create_edit_changed(content);
    if (application_guild_lifecycle_active())
        return application_guild_lifecycle_edit_changed(content);
    if (application_profile_editor_active())
        return application_profile_editor_edit_changed(content);
    if (application_guild_community_active())
        return application_guild_community_edit_changed(content);
    if (application_report_active())
        return false;
    if (application_custom_status_active())
        return application_custom_status_edit_changed(content);
    return owner && owner->scene.active && settings_layer_edit_changed(&owner->layer, content);
}
u64 application_channel_overview_next_wake() noexcept {
    if (const u64 wake = application_guild_settings_next_wake())
        return wake;
    if (const u64 wake = application_poll_create_next_wake())
        return wake;
    if (const u64 wake = application_guild_lifecycle_next_wake())
        return wake;
    if (const u64 wake = application_profile_editor_next_wake())
        return wake;
    if (const u64 wake = application_guild_community_next_wake())
        return wake;
    if (const u64 wake = application_report_next_wake())
        return wake;
    // User notes wait out a typing pause; never let that hide the modal's work.
    const u64 social = application_user_actions_next_wake();
    if (!owner || g_app.details_request_owner != MessageHistoryRequestOwner::none)
        return social;
    const u64 controller = channel_overview_controller_next_wake(owner->controller);
    const u64 own = owner->queued ? (owner->retry_at ? owner->retry_at : 1) : 0;
    const u64 wake = controller && own ? minimum(controller, own) : controller ? controller : own;
    return wake && social ? minimum(wake, social) : wake ? wake : social;
}
u64 application_channel_overview_caret_wake() noexcept {
    if (application_guild_settings_active())
        return application_guild_settings_caret_wake();
    if (application_poll_create_active())
        return application_poll_create_caret_wake();
    if (application_guild_lifecycle_active())
        return application_guild_lifecycle_caret_wake();
    if (application_profile_editor_active())
        return application_profile_editor_caret_wake();
    if (application_guild_community_active())
        return application_guild_community_caret_wake();
    if (application_report_active())
        return application_report_caret_wake();
    if (application_custom_status_active())
        return application_custom_status_caret_wake();
    return owner && owner->scene.active ? settings_layer_next_wake(owner->layer) : 0;
}
void application_channel_overview_tick(u64 now) noexcept {
    application_guild_settings_tick(now);
    application_poll_create_tick(now);
    application_guild_lifecycle_tick(now);
    application_profile_editor_tick(now);
    application_guild_community_tick(now);
    application_report_tick(now);
    application_custom_status_dialog_tick(now);
    if (owner && owner->scene.active)
        settings_layer_tick(&owner->layer, now, g_app.preferences.reduced_motion);
}
u32 application_channel_overview_owner_bytes() noexcept {
    return (owner ? sizeof(*owner) : 0) + application_guild_settings_owner_bytes() +
           application_poll_create_owner_bytes() + application_custom_status_owner_bytes() +
           application_guild_lifecycle_owner_bytes() + application_guild_community_owner_bytes() +
           application_profile_editor_owner_bytes() + application_report_owner_bytes();
}
bool application_channel_overview_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    if (application_guild_settings_active())
        return application_guild_settings_input(kind, key, repeated);
    if (application_poll_create_active())
        return application_poll_create_input(kind, key, repeated);
    if (application_guild_lifecycle_active())
        return application_guild_lifecycle_input(kind, key, repeated);
    if (application_profile_editor_active())
        return application_profile_editor_input(kind, key, repeated);
    if (application_guild_community_active())
        return application_guild_community_input(kind, key, repeated);
    if (application_report_active())
        return application_report_input(kind, key, repeated);
    if (application_custom_status_active())
        return application_custom_status_input(kind, key, repeated);
    return owner && settings_layer_input(&owner->layer, kind, key, repeated);
}

SettingsLayer* application_channel_overview_settings_layer() noexcept {
    return owner ? &owner->layer : nullptr;
}

bool application_channel_overview_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (application_guild_settings_active())
        return application_guild_settings_window_message(message, wparam, lparam);
    if (application_poll_create_active())
        return application_poll_create_window_message(message, wparam, lparam);
    if (application_guild_lifecycle_active())
        return application_guild_lifecycle_window_message(message, wparam, lparam);
    if (application_profile_editor_active())
        return application_profile_editor_window_message(message, wparam, lparam);
    if (application_guild_community_active())
        return application_guild_community_window_message(message, wparam, lparam);
    if (application_report_active())
        return application_report_window_message(message, wparam, lparam);
    if (application_custom_status_active())
        return application_custom_status_window_message(message, wparam, lparam);
    return owner && owner->scene.active && settings_layer_window_message(&owner->layer, message, wparam, lparam);
}
} // namespace maicord::app_detail
