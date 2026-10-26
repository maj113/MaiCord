#include "application_guild_settings_internal.hpp"
#include "maicord/core/memory.hpp"
#include "application_toast.hpp"
#include "application_account_presentation.hpp"
#include "application_channel_session.hpp"
#include "application_content_input.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/permission_compute.hpp"
#include "maicord/model/permission_grid.hpp"
#include "maicord/ui/icon.hpp"
#include "account_ui_runtime.hpp"

namespace maicord::app_detail::server_settings {
namespace {

// Role editor tabs.
enum RoleTab : u8 {
    role_tab_display,
    role_tab_permissions,
};
constexpr Utf8View kRoleTabs[]{utf8_literal("Display"), utf8_literal("Permissions")};

bool role_dirty() noexcept {
    return owner->editing_role &&
           (!guild_role_draft_equal(owner->role_base, owner->role_draft) || pending(owner->role_icon));
}
bool role_writable() noexcept {
    const auto* r = role(owner->editing_role);
    return r && !owner->role_missing && guild_settings_role_editable(owner->access, *r);
}
void load_order() noexcept {
    const RoleRecord* sorted[kGuildRoleMaximum];
    owner->order_count = guild_settings_sorted_roles(state(), owner->guild_id, {sorted, kGuildRoleMaximum});
    for (u32 i = 0; i < owner->order_count; ++i)
        owner->order[i] = sorted[i]->id;
    owner->order_dirty = false;
}

__declspec(noinline) void load_role_editor(const RoleRecord& r) noexcept {
    copy_value(&owner->editing_role, r.id);
    owner->role_draft = guild_role_draft_from(state(), r);
    copy_value(&owner->role_base, owner->role_draft);
    owner->role_conflict = owner->role_echo = owner->role_missing = owner->hex_invalid = false;
    set_editor(editor_name, {owner->role_draft.name, owner->role_draft.name_bytes});
    char hex[8];
    const u32 bytes = owner->role_draft.color ? guild_role_color_format(owner->role_draft.color, {hex, 8}) : 0;
    set_editor(editor_hex, {hex, bytes});
}
bool role_patch_pending() noexcept {
    RolePatch patch{};
    return guild_role_patch(owner->role_base, owner->role_draft, &patch);
}
bool order_matches_store() noexcept {
    return !guild_settings_order_changed(state(), owner->guild_id, {owner->order, owner->order_count});
}
Utf8View lock_text(GuildRoleLock lock) noexcept {
    switch (lock) {
    case GuildRoleLock::no_permission:
        return utf8_literal("Role is locked because you don't have the Manage Roles permission.");
    case GuildRoleLock::highest:
        return utf8_literal("Role is locked because it is your highest role. Ask a higher role or the owner.");
    case GuildRoleLock::above:
        return utf8_literal("Role is locked because it ranks above your highest role.");
    default:
        return {};
    }
}
u32 member_count(u64 role_id) noexcept {
    for (u32 j = 0; j < owner->counts; ++j)
        if (owner->count_role[j] == role_id)
            return owner->count_value[j];
    return ~0u;
}

// Roles: the @everyone card, search with Create Role, the column header and
// one table row per role (draggable below the account's highest role).
void project_role_list(Bank& bank) noexcept {
    const bool manage = page_allowed(Page::roles) && editable();
    const auto query = editor_text(editor_search);
    if (const auto* everyone = role(owner->guild_id)) {
        const u32 start = bank.used;
        bank.add(chat_state_store_role_name(state(), *everyone));
        bank.add(" · applies to all server members");
        const u32 index = push_row(control_everyone, utf8_literal("Default permissions"), bank.since(start),
                                   UiSettingsControlKind::action, true);
        if (index != ~0u)
            owner->list_items[index] = {UiSettingsListStyle::card, UiIcon::members, UiIcon::count,
                                        {UiIcon::count, UiIcon::count}, false};
    }
    push_editor(control_search, {}, {}, UiSettingsControlKind::edit, editable(), editor_search,
                utf8_literal("Search roles"), manage && !owner->order_dirty ? utf8_literal("Create Role") : Utf8View{});
    u32 listed = 0;
    for (u32 i = 0; i < owner->order_count; ++i)
        listed += owner->order[i] != owner->guild_id && role(owner->order[i]);
    static const Utf8View members_column[]{utf8_literal("MEMBERS")};
    const u32 heading = bank.used;
    bank.add("ROLES — ");
    bank.number(listed);
    const u32 header = push_choices(control_list_category, bank.since(heading),
                                    manage ? utf8_literal("Members use the colour of the highest role they have on this "
                                                          "list. Drag roles to reorder them.")
                                           : Utf8View{},
                                    UiSettingsControlKind::category, true, {members_column, 1});
    if (header != ~0u)
        owner->list_items[header] = {UiSettingsListStyle::header, UiIcon::count, UiIcon::count,
                                     {UiIcon::count, UiIcon::count}, false};
    if (owner->status.bytes && owner->failed)
        push_error({}, owner->status);
    else if (busy() && owner->request.op == Op::role_create)
        push_loading(utf8_literal("Creating role…"));
    // Row ids follow the draft order so drops and activation map back to it.
    u32 shown = 0;
    for (u32 i = 0; i < owner->order_count; ++i) {
        const auto* resolved = role(owner->order[i]);
        if (!resolved || resolved->id == owner->guild_id)
            continue;
        const auto& r = *resolved;
        const auto name = chat_state_store_role_name(state(), r);
        if (!guild_settings_text_matches(query, name))
            continue;
        const bool editable_role = manage && guild_settings_role_editable(owner->access, r);
        const u32 members = member_count(r.id);
        const u32 start = bank.used;
        if (members != ~0u)
            bank.number(members);
        owner->chips[i][0] = bank.since(start);
        u8 flags = ui_settings_presentation_accent;
        if (editable_role && !query.bytes)
            flags |= ui_settings_presentation_grip;
        const u32 index = push_choices(kRowBase + i, bank.add(name), {}, UiSettingsControlKind::action, true,
                                       {owner->chips[i], 1}, 0, {},
                                       {UiSettingsVariant::standard, flags, kUiSettingsNoDefault});
        if (index != ~0u)
            owner->list_items[index] = {UiSettingsListStyle::row, UiIcon::shield, UiIcon::user,
                                        {editable_role ? UiIcon::edit : UiIcon::eye, UiIcon::ellipsis},
                                        guild_settings_role_lock(owner->access, r) != GuildRoleLock::none ||
                                            r.managed};
        owner->row_accent[i] = guild_role_color_rgba(r.color);
        ++shown;
    }
    if (!shown)
        push_notice(query.bytes ? utf8_literal("No roles match your search.")
                                : utf8_literal("Create a role to group members and give them permissions."));
}

Utf8View permission_lock_text(GuildPermissionLock lock) noexcept {
    return lock == GuildPermissionLock::missing ? utf8_literal("You can't change this: none of your roles have it.")
           : lock == GuildPermissionLock::self  ? utf8_literal("You can't change this: removing it would remove it "
                                                               "from you.")
                                                : Utf8View{};
}

// Permissions tab: search, Clear permissions and the grouped switches. A
// switch the account may not change is disabled with its reason.
void project_permissions(Bank& bank, bool writable) noexcept {
    auto& d = owner->role_draft;
    push_editor(control_search, {}, {}, UiSettingsControlKind::edit, true, editor_search,
                utf8_literal("Search permissions"));
    const bool clearable = writable && guild_role_can_clear(state(), owner->access, d);
    push_row(control_clear, utf8_literal("Clear permissions"),
             d.permissions && writable && !clearable
                 ? utf8_literal("You can't clear these: it would remove permissions from you.")
                 : utf8_literal("Turns off every permission of this role."),
             UiSettingsControlKind::action, clearable, false, utf8_literal("Clear"),
             {UiSettingsVariant::standard, 0, kUiSettingsNoDefault});
    const auto query = editor_text(editor_search);
    const auto entries = permission_grid_entries();
    const PermissionSet grantable = guild_settings_grantable(owner->access);
    u32 found = 0;
    for (u32 group = 0; group < static_cast<u32>(PermissionGroup::count); ++group) {
        bool first = true;
        for (u32 i = 0; i < entries.count; ++i) {
            const auto& entry = entries[i];
            if (static_cast<u32>(entry.group) != group || !permission_grid_applies(entry, permission_scope_guild) ||
                (query.bytes && !guild_settings_text_matches(query, entry.label) &&
                 !guild_settings_text_matches(query, entry.description)))
                continue;
            if (first) {
                push_category(control_permission_category + group,
                              permission_group_label(static_cast<PermissionGroup>(group)));
                first = false;
            }
            const bool on = (d.permissions & entry.bit) != 0;
            const auto lock = writable ? guild_role_permission_lock(state(), owner->access, d, entry.bit)
                                       : GuildPermissionLock::none;
            Utf8View description = entry.description;
            if (lock != GuildPermissionLock::none) {
                const u32 start = bank.used;
                bank.add(entry.description);
                bank.add(" ");
                bank.add(permission_lock_text(lock));
                description = bank.since(start);
            }
            push_row(control_permission + i, entry.label, description, UiSettingsControlKind::toggle,
                     writable && lock == GuildPermissionLock::none && (on || (grantable & entry.bit) == entry.bit),
                     on);
            ++found;
        }
    }
    if (!found)
        push_notice(utf8_literal("No permissions found."));
}

void project_display(Bank& bank, const RoleRecord* r, bool writable) noexcept {
    auto& d = owner->role_draft;
    push_editor(control_name, utf8_literal("Role name"), {}, UiSettingsControlKind::edit,
                writable && !(r && r->managed), editor_name, utf8_literal("Role name"));
    auto& color_names = owner->color_names;
    color_names[0] = utf8_literal("Default");
    u32 selected = d.color ? kUiSettingsNoChoice : 0;
    for (u32 i = 0; i < kGuildRoleColorCount; ++i) {
        color_names[i + 1] = utf8_literal("Colour");
        if (kGuildRoleColors[i] == d.color)
            selected = i + 1;
    }
    push_choices(control_color, utf8_literal("Role colour"),
                 utf8_literal("Members use the colour of the highest role they have on the roles list."),
                 UiSettingsControlKind::swatches, writable, {color_names, kGuildRoleColorCount + 1}, selected, {},
                 {UiSettingsVariant::standard, 0, kUiSettingsNoDefault});
    push_editor(control_hex, utf8_literal("Custom colour"),
                owner->hex_invalid ? utf8_literal("Use six hex digits, like #3498DB.") : Utf8View{},
                UiSettingsControlKind::edit, writable, editor_hex, utf8_literal("#000000"));
    push_row(control_hoist, utf8_literal("Show members separately"),
             utf8_literal("List members with this role in their own group in the member list."),
             UiSettingsControlKind::toggle, writable, d.hoist);
    push_row(control_mentionable, utf8_literal("Allow anyone to @mention this role"),
             utf8_literal("Members with the right permission can always mention it."), UiSettingsControlKind::toggle,
             writable, d.mentionable);
    if (guild_role_icons_available(premium_tier(), guild_features())) {
        const bool has_icon = r && r->has_icon;
        push_row(control_role_icon, utf8_literal("Role icon"),
                 picked_text(bank, owner->role_icon,
                             utf8_literal("Shown next to member names. PNG, JPEG or WebP, at least 64×64.")),
                 UiSettingsControlKind::action, writable && connected() && !(r && r->managed), false,
                 utf8_literal("Upload icon"));
        if (has_icon || pending(owner->role_icon))
            push_row(control_role_icon_remove, utf8_literal("Remove role icon"), {}, UiSettingsControlKind::action,
                     writable && (owner->role_icon.set || (has_icon && !owner->role_icon.clear)), false,
                     owner->role_icon.set && !has_icon ? utf8_literal("Discard") : utf8_literal("Remove"),
                     {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
    }
    if (r && guild_settings_can_view_as_role(owner->access, *r)) {
        push_category(control_display_category, utf8_literal("View Server as Role"));
        push_row(control_view_as, utf8_literal("Preview this role"),
                 utf8_literal("Test what this role can do and which channels it can see. Voice restrictions such "
                              "as priority speaker aren't reflected."),
                 UiSettingsControlKind::action, true, false, utf8_literal("View Server as Role"));
    }
    if (r && guild_settings_role_deletable(owner->access, *r)) {
        push_category(control_delete_category, utf8_literal("Delete role"));
        push_row(control_delete, utf8_literal("Delete this role"),
                 utf8_literal("Members lose the role and its permissions. This can't be undone."),
                 UiSettingsControlKind::action, writable, false, utf8_literal("Delete role"),
                 {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    }
}

void project_role_editor(Bank& bank) noexcept {
    const auto* r = role(owner->editing_role);
    const bool everyone = owner->editing_role == owner->guild_id;
    const bool writable = editable() && role_writable();
    const u32 start = bank.used;
    if (owner->role_missing)
        bank.add("This role was deleted.");
    else if (r && !role_writable())
        bank.add(lock_text(guild_settings_role_lock(owner->access, *r)));
    else if (everyone)
        bank.add("Default permissions for all server members.");
    else if (r && r->managed)
        bank.add("Managed by an app: its name can't change and it can't be deleted.");
    else
        bank.add("Editing role");
    const auto header_description = bank.since(start);
    const auto header_name = bank.add({owner->role_draft.name, owner->role_draft.name_bytes});
    push_row(control_header, header_name, header_description, UiSettingsControlKind::action, true, false,
             utf8_literal("Back to roles"),
             {UiSettingsVariant::standard, static_cast<u8>(everyone ? 0 : ui_settings_presentation_accent),
              kUiSettingsNoDefault});
    if (owner->role_conflict)
        push_error(utf8_literal("Changed elsewhere"),
                   utf8_literal("Someone else updated this role. Load the latest values or keep editing."),
                   utf8_literal("Load latest"));
    // @everyone has permissions only.
    const u8 tab = everyone ? role_tab_permissions : owner->role_tab;
    if (!everyone)
        push_choices(control_role_tabs, utf8_literal("Role settings"), {}, UiSettingsControlKind::tabs, true,
                     {kRoleTabs, 2}, tab);
    if (tab == role_tab_permissions)
        project_permissions(bank, writable);
    else
        project_display(bank, r, writable);
}

void open_role(u64 id, u8 tab = role_tab_display) noexcept {
    const auto* r = role(id);
    if (!r || guard_dirty())
        return;
    release_picked(&owner->role_icon);
    load_role_editor(*r);
    owner->role_tab = tab;
    set_editor(editor_search, {});
    owner->scene.content_offset = 0;
    owner->scene.focus = {UiSettingsTargetKind::control, page_key(Page::roles), control_header};
    repaint();
}
void close_role() noexcept {
    if (guard_dirty())
        return;
    owner->editing_role = 0;
    release_picked(&owner->role_icon);
    set_editor(editor_search, {});
    owner->scene.content_offset = 0;
    repaint();
}
// The role list's menu.
enum RoleAction : u32 {
    role_action_copy_id = 1,
    role_action_delete,
    role_action_edit,
    role_action_view_as,
};
void confirm_delete_role(u64 role_id) noexcept {
    const auto* r = role(role_id);
    if (!r || !guild_settings_role_deletable(owner->access, *r))
        return;
    static char context[256];
    Bank bank{context, sizeof(context), 0};
    bank.add("Are you sure you want to delete the ");
    bank.add(chat_state_store_role_name(state(), *r));
    bank.add(" role? This action cannot be undone.");
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Delete Role");
    request.context = bank.since(0);
    request.accept = utf8_literal("Delete");
    request.owner = form_key(form_role_delete, r->id);
    request.valid = form_valid;
    request.save = form_save;
    request.confirmation_only = true;
    request.danger = true;
    application_text_form_open(request);
}
// Closes Server Settings into the view; the settings owner is gone after.
void view_as(u64 role_id) noexcept {
    const auto* r = role(role_id);
    if (!r || !guild_settings_can_view_as_role(owner->access, *r) || guard_dirty())
        return;
    application_view_as_role_start(owner->guild_id, role_id);
}
void open_role_menu(u64 role_id, const UiRect& anchor) noexcept {
    auto& p = *owner;
    const auto* r = role(role_id);
    if (!r)
        return;
    u32 count = 0;
    const bool editable_role = page_allowed(Page::roles) && guild_settings_role_editable(p.access, *r);
    popup_row(count++, editable_role ? utf8_literal("Edit Role") : utf8_literal("View Role"), role_action_edit);
    if (guild_settings_can_view_as_role(p.access, *r))
        popup_row(count++, utf8_literal("View Server as Role"), role_action_view_as);
    if (g_app.preferences.developer_mode)
        popup_row(count++, utf8_literal("Copy Role ID"), role_action_copy_id);
    if (page_allowed(Page::roles) && guild_settings_role_deletable(p.access, *r)) {
        popup_row(count, utf8_literal("Delete Role"), role_action_delete,
                  popup_item_danger | popup_item_separator_before);
        ++count;
    }
    p.menu = Menu::role;
    p.menu_item = role_id;
    settings_layer_open_choice(&p.layer, role_id, {p.popup_items, count}, anchor, 240);
}

void load() noexcept {
    release_picked(&owner->role_icon);
    if (owner->editing_role) {
        if (const auto* r = role(owner->editing_role))
            load_role_editor(*r);
    } else
        load_order();
}

void project(Bank& bank) noexcept {
    if (owner->editing_role)
        project_role_editor(bank);
    else
        project_role_list(bank);
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    const auto target = activation.value.target;
    const u64 id = target.control_id;
    if (!p.editing_role) {
        if (id == control_search) {
            // The Create Role button beside the search field.
            if (!p.order_dirty && !guard_dirty())
                queue(Op::role_create);
            repaint();
        } else if (id == control_everyone)
            open_role(p.guild_id, role_tab_permissions);
        else if (id >= kRowBase && id < kRowBase + p.order_count) {
            const u64 role_id = p.order[id - kRowBase];
            const u32 part = activation.pointer ? activation.value.selected_choice : ui_settings_list_row;
            UiRect anchor = activation.anchor;
            if (part == ui_settings_list_action + 1) {
                ui_settings_scene_choice_rectangle(p.scene, target, part, &anchor);
                open_role_menu(role_id, anchor);
            } else
                open_role(role_id);
        }
        return;
    }
    auto& d = p.role_draft;
    if (id == control_header) {
        if (activation.button || !activation.pointer)
            close_role();
        return;
    }
    if (id == control_role_tabs) {
        if (activation.value.selected_choice < 2 && activation.value.selected_choice != p.role_tab) {
            p.role_tab = static_cast<u8>(activation.value.selected_choice);
            set_editor(editor_search, {});
            p.scene.content_offset = 0;
            repaint();
        }
        return;
    }
    if (id == control_view_as) {
        view_as(p.editing_role);
        return;
    }
    if (!role_writable())
        return;
    if (id == control_role_icon) {
        start_pick(Pick::role_icon);
        return;
    }
    if (id == control_role_icon_remove) {
        const auto* r = role(p.editing_role);
        release_picked(&p.role_icon);
        p.role_icon.clear = r && r->has_icon;
        repaint();
        return;
    }
    if (id == control_clear) {
        if (guild_role_can_clear(state(), p.access, d))
            d.permissions = 0;
        repaint();
        return;
    }
    if (id == control_delete)
        confirm_delete_role(p.editing_role);
    else if (id == control_hoist)
        d.hoist = activation.value.checked;
    else if (id == control_mentionable)
        d.mentionable = activation.value.checked;
    else if (id == control_color && activation.value.selected_choice <= kGuildRoleColorCount) {
        d.color = activation.value.selected_choice ? kGuildRoleColors[activation.value.selected_choice - 1] : 0;
        char hex[8];
        set_editor(editor_hex, {hex, d.color ? guild_role_color_format(d.color, {hex, 8}) : 0});
        p.hex_invalid = false;
    } else if (id >= control_permission && id < control_permission + permission_grid_entries().count) {
        const auto& entry = permission_grid_entries()[static_cast<u32>(id - control_permission)];
        if (guild_role_permission_lock(state(), p.access, d, entry.bit) != GuildPermissionLock::none ||
            !guild_role_toggle(&d, p.access, entry.bit, activation.value.checked))
            return;
    }
    if (activation.value.kind == UiSettingsControlKind::toggle)
        ui_settings_scene_animate_toggle(&p.scene, target, activation.value.checked, clock_monotonic_milliseconds());
    repaint();
}

bool dirty() noexcept {
    return owner->editing_role ? role_dirty() : owner->order_dirty;
}

bool can_save() noexcept {
    if (!owner->editing_role)
        return owner->order_dirty;
    return !owner->role_conflict && role_writable() && !owner->hex_invalid &&
           (owner->editing_role == owner->guild_id ||
            guild_role_name_valid({owner->role_draft.name, owner->role_draft.name_bytes})) &&
           (role_patch_pending() || pending(owner->role_icon));
}

Op next_save() noexcept {
    if (!owner->editing_role)
        return owner->order_dirty ? Op::role_reorder : Op::none;
    return role_patch_pending() ? Op::role_update : pending(owner->role_icon) ? Op::role_icon : Op::none;
}

bool reconcile(u64 now) noexcept {
    auto& p = *owner;
    bool changed = false;
    if (p.editing_role) {
        const auto* r = role(p.editing_role);
        if (!r) {
            changed = !p.role_missing;
            p.role_missing = true;
        } else if (p.role_echo) {
            const auto& current = guild_role_draft_from(state(), *r);
            if (guild_role_draft_equal(current, p.role_base) || deadline_due(now, p.role_echo_until))
                p.role_echo = false;
        } else if (!mutation_pending()) {
            const auto& current = guild_role_draft_from(state(), *r);
            if (!guild_role_draft_equal(current, p.role_base)) {
                if (role_dirty())
                    changed = !p.role_conflict, p.role_conflict = true;
                else {
                    load_role_editor(*r);
                    changed = true;
                }
            }
        }
    }
    if (!p.editing_role && !p.order_dirty && !mutation_pending() && !order_matches_store()) {
        load_order();
        changed = true;
    }
    if (p.open_after_create && role(p.open_after_create) && !p.editing_role) {
        const u64 id = p.open_after_create;
        p.open_after_create = 0;
        load_order();
        open_role(id);
        changed = true;
    }
    return changed;
}

bool ensure_data() noexcept {
    if (owner->counts_requested)
        return false;
    owner->counts_requested = true;
    queue(Op::role_counts);
    return true;
}

void notice_action() noexcept {
    if (owner->role_conflict)
        if (const auto* r = role(owner->editing_role))
            load_role_editor(*r);
}

Utf8View invalid() noexcept {
    const auto& p = *owner;
    return p.editing_role && p.editing_role != p.guild_id &&
                   !guild_role_name_valid({p.role_draft.name, p.role_draft.name_bytes})
               ? utf8_literal("Role names need 1–100 characters.")
               : Utf8View{};
}

} // namespace

u32 resolve_order(const RoleRecord** output) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < owner->order_count; ++i)
        if (const auto* r = role(owner->order[i]))
            output[count++] = r;
    return count;
}

void roles_drop(u64 dragged, u64 target, bool after) noexcept {
    if (!owner || owner->page != Page::roles || owner->editing_role || !editable() ||
        editor_text(editor_search).bytes || dragged < kRowBase || target < kRowBase)
        return;
    auto& p = *owner;
    const u32 from = static_cast<u32>(dragged - kRowBase), at = static_cast<u32>(target - kRowBase);
    if (from >= p.order_count || at >= p.order_count)
        return;
    u32 to = after ? at + 1 : at;
    if (to > from)
        --to;
    const RoleRecord* order[kGuildRoleMaximum];
    for (u32 i = 0; i < p.order_count; ++i) {
        order[i] = role(p.order[i]);
        if (!order[i])
            return;
    }
    if (!guild_settings_role_move(p.access, {order, p.order_count}, from, to))
        return;
    for (u32 i = 0; i < p.order_count; ++i)
        p.order[i] = order[i]->id;
    // A move back to the stored order clears the draft.
    p.order_dirty = !order_matches_store();
    repaint();
}

namespace {
void context(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    const u64 id = activation.value.target.control_id;
    if (p.editing_role || id < kRowBase || id >= kRowBase + p.order_count)
        return;
    open_role_menu(p.order[id - kRowBase], activation.anchor);
}

void choose(Menu menu, u32 action) noexcept {
    auto& p = *owner;
    if (menu != Menu::role || action < kMenuChoiceBase)
        return;
    switch (action - kMenuChoiceBase) {
    case role_action_copy_id: {
        char digits[24];
        Bank bank{digits, sizeof(digits), 0};
        bank.number(p.menu_item);
        application_copy_text(bank.since(0), kToastIdCopied, kToastIdCopyFailed);
        break;
    }
    case role_action_delete:
        if (!busy())
            confirm_delete_role(p.menu_item);
        break;
    case role_action_edit:
        open_role(p.menu_item);
        break;
    case role_action_view_as:
        view_as(p.menu_item);
        break;
    default:
        break;
    }
}
} // namespace

const PageOps kRolesPage{load,      project,   activate,    choose,        dirty,  can_save,
                         next_save, reconcile, ensure_data, notice_action, invalid, context};

} // namespace maicord::app_detail::server_settings

namespace maicord::app_detail {
namespace {
bool refresh_view() noexcept {
    const bool ok = synchronize_navigation_view() && synchronize_selected_channel() && refresh_account_text(true);
    application_request_frame();
    return ok;
}
} // namespace

bool application_view_as_role_start(u64 guild_id, u64 role_id) noexcept {
    if (!g_app.account_ui)
        return false;
    const u64 roles[]{role_id};
    if (!chat_state_store_view_as_roles(&g_app.account_ui->chat_state, guild_id,
                                        {roles, role_id && role_id != guild_id ? 1u : 0u}))
        return false;
    if (application_guild_settings_active())
        application_guild_settings_close();
    // The guild's remembered channel, or one the viewed roles can see.
    select_content_guild(guild_id);
    return refresh_view();
}

bool application_view_as_role_end(u64 guild_id, bool back_to_settings) noexcept {
    if (!g_app.account_ui || !chat_state_store_end_view_as(&g_app.account_ui->chat_state, guild_id))
        return false;
    const bool ok = refresh_view();
    if (back_to_settings)
        application_guild_settings_open(guild_id, static_cast<u8>(server_settings::Page::roles));
    return ok;
}

UiBannerNotice application_view_as_role_notice(u64 channel_id, Slice<char> text) noexcept {
    UiBannerNotice notice{};
    if (!g_app.account_ui || !text.data)
        return notice;
    const auto& s = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(s, channel_id);
    if (!channel || !channel->guild_id || !chat_state_store_viewing_as(s, channel->guild_id))
        return notice;
    u64 roles[kGuildRoleMaximum];
    const u32 count = chat_state_store_view_roles(s, channel->guild_id, {roles, kGuildRoleMaximum});
    TextBank bank{text.data, text.count, 0};
    bank.add("You are viewing this server as ");
    if (count == 1) {
        if (const auto* r = chat_state_store_find_role(s, channel->guild_id, roles[0]))
            bank.add(chat_state_store_role_name(s, *r));
    } else if (!count)
        bank.add("@everyone");
    else {
        bank.number(count);
        bank.add(" roles");
    }
    bank.add(".");
    notice.id = channel->guild_id ^ 0x5649455741535249ull;
    notice.title = bank.since(0);
    notice.description = utf8_literal("Channels and actions follow these roles. Voice restrictions aren't reflected.");
    notice.primary = utf8_literal("Back to Roles");
    notice.secondary = utf8_literal("Exit");
    notice.icon = UiIcon::eye;
    return notice;
}

} // namespace maicord::app_detail
