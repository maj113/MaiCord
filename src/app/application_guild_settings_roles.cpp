#include "application_guild_settings_internal.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/permission_grid.hpp"
#include "maicord/ui/clipboard.hpp"

namespace maicord::app_detail::server_settings {
namespace {

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

void load_role_editor(const RoleRecord& r) noexcept {
    owner->editing_role = r.id;
    owner->role_base = owner->role_draft = guild_role_draft_from(state(), r);
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
// Whether the draft order still is the stored one.
bool order_matches_store() noexcept {
    const RoleRecord* stored[kGuildRoleMaximum];
    const u32 count = guild_settings_sorted_roles(state(), owner->guild_id, {stored, kGuildRoleMaximum});
    bool same = count == owner->order_count;
    for (u32 i = 0; same && i < count; ++i)
        same = stored[i]->id == owner->order[i];
    return same;
}

void project_role_list(Bank& bank) noexcept {
    const bool manage = page_allowed(Page::roles) && editable();
    push({control_create, utf8_literal("Roles"),
          utf8_literal("Members combine the permissions of all their roles. Drag roles to change their order."),
          UiSettingsControlKind::action, manage && !owner->order_dirty, false, {}, 0, utf8_literal("Create role"),
          nullptr},
         {UiSettingsVariant::primary, 0, kUiSettingsNoDefault});
    push({control_search, utf8_literal("Search roles"), {}, UiSettingsControlKind::edit, editable(), false, {}, 0,
          {}, &editor_view(editor_search, control_search, utf8_literal("Search roles"), editable())});
    const RoleRecord* sorted[kGuildRoleMaximum];
    const u32 count = resolve_order(sorted);
    const auto query = editor_text(editor_search);
    const u32 heading = bank.used;
    bank.add(utf8_literal("Roles — "));
    bank.number(count);
    push(category(control_list_category, bank.since(heading)));
    if (owner->status.bytes && owner->failed)
        push(notice({}, owner->status), {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    else if (busy() && owner->request.op == Op::role_create)
        push(loading(utf8_literal("Creating role…")));
    else if (!owner->counts && busy() && owner->request.op == Op::role_counts)
        push(loading(utf8_literal("Loading member counts…")));
    // Row ids follow the draft order so drops and activation map back to it.
    for (u32 i = 0; i < owner->order_count; ++i) {
        const auto* resolved = role(owner->order[i]);
        if (!resolved)
            continue;
        const auto& r = *resolved;
        const auto name = chat_state_store_role_name(state(), r);
        if (!guild_settings_text_matches(query, name))
            continue;
        const bool everyone = r.id == owner->guild_id;
        const bool editable_role = guild_settings_role_editable(owner->access, r);
        const u32 start = bank.used;
        if (everyone)
            bank.add(utf8_literal("Default permissions for all members"));
        else {
            u32 members = ~0u;
            for (u32 j = 0; j < owner->counts; ++j)
                if (owner->count_role[j] == r.id)
                    members = owner->count_value[j];
            if (members != ~0u) {
                bank.number(members);
                bank.add(members == 1 ? utf8_literal(" member") : utf8_literal(" members"));
            }
            if (r.managed)
                bank.add(members != ~0u ? utf8_literal(" · Managed by an app") : utf8_literal("Managed by an app"));
        }
        const auto description = bank.since(start);
        u8 flags = ui_settings_presentation_compact;
        if (manage && editable_role && !everyone && !query.bytes)
            flags |= ui_settings_presentation_grip;
        push({kRowBase + i, bank.add(name), description, UiSettingsControlKind::action, true, false, {}, 0,
              editable_role && manage ? utf8_literal("Edit") : utf8_literal("View"), nullptr},
             {UiSettingsVariant::standard, static_cast<u8>(flags | (everyone ? 0 : ui_settings_presentation_accent)),
              kUiSettingsNoDefault});
        owner->row_accent[i] = everyone ? 0u : guild_role_color_rgba(r.color);
    }
}

void project_role_editor(Bank& bank) noexcept {
    const auto* r = role(owner->editing_role);
    const bool everyone = owner->editing_role == owner->guild_id;
    const bool writable = editable() && role_writable();
    auto& d = owner->role_draft;
    const u32 start = bank.used;
    if (owner->role_missing)
        bank.add(utf8_literal("This role was deleted."));
    else if (!role_writable())
        bank.add(utf8_literal("Read-only: this role is not below your highest role."));
    else if (everyone)
        bank.add(utf8_literal("Applies to every member of the server."));
    else
        bank.add(utf8_literal("Editing role"));
    const auto header_description = bank.since(start);
    const auto header_name = bank.add({d.name, d.name_bytes});
    push({control_header, header_name, header_description, UiSettingsControlKind::action, true,
          false, {}, 0, utf8_literal("Back to roles"), nullptr},
         {UiSettingsVariant::standard, static_cast<u8>(everyone ? 0 : ui_settings_presentation_accent),
          kUiSettingsNoDefault});
    if (owner->role_conflict)
        push(notice(utf8_literal("Changed elsewhere"),
                    utf8_literal("Someone else updated this role. Load the latest values or keep editing."),
                    utf8_literal("Load latest")),
             {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    if (!everyone) {
        push(category(control_display_category, utf8_literal("Display")));
        push({control_name, utf8_literal("Role name"), {}, UiSettingsControlKind::edit,
              writable && !(r && r->managed), false, {}, 0, {},
              &editor_view(editor_name, control_name, utf8_literal("Role name"), writable && !(r && r->managed))});
        auto& color_names = owner->color_names;
        color_names[0] = utf8_literal("Default");
        u32 selected = d.color ? kUiSettingsNoChoice : 0;
        for (u32 i = 0; i < kGuildRoleColorCount; ++i) {
            color_names[i + 1] = utf8_literal("Colour");
            if (kGuildRoleColors[i] == d.color)
                selected = i + 1;
        }
        push({control_color, utf8_literal("Role colour"),
              utf8_literal("Members show the colour of their highest coloured role."), UiSettingsControlKind::swatches,
              writable, false, {color_names, kGuildRoleColorCount + 1}, selected, {}, nullptr},
             {UiSettingsVariant::standard, 0, kUiSettingsNoDefault});
        push({control_hex, utf8_literal("Custom colour"),
              owner->hex_invalid ? utf8_literal("Use six hex digits, like #3498DB.") : Utf8View{},
              UiSettingsControlKind::edit, writable, false, {}, 0, {},
              &editor_view(editor_hex, control_hex, utf8_literal("#000000"), writable)});
        push({control_hoist, utf8_literal("Show members separately"),
              utf8_literal("List members with this role in their own group in the member list."),
              UiSettingsControlKind::toggle, writable, d.hoist, {}, 0, {}, nullptr});
        push({control_mentionable, utf8_literal("Allow anyone to mention this role"),
              utf8_literal("Members with the right permission can always mention it."), UiSettingsControlKind::toggle,
              writable, d.mentionable, {}, 0, {}, nullptr});
        if (guild_role_icons_available(premium_tier(), guild_features())) {
            const bool has_icon = r && r->has_icon;
            push({control_role_icon, utf8_literal("Role icon"),
                  picked_text(bank, owner->role_icon,
                              utf8_literal("Shown next to member names. PNG, JPEG or WebP, at least 64×64.")),
                  UiSettingsControlKind::action, writable && connected() && !(r && r->managed), false, {}, 0,
                  utf8_literal("Upload icon"), nullptr});
            if (has_icon || pending(owner->role_icon))
                push({control_role_icon_remove, utf8_literal("Remove role icon"), {}, UiSettingsControlKind::action,
                      writable && (owner->role_icon.set || (has_icon && !owner->role_icon.clear)), false, {}, 0,
                      owner->role_icon.set && !has_icon ? utf8_literal("Discard") : utf8_literal("Remove"), nullptr},
                     {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
        }
    }
    const auto entries = permission_grid_entries();
    const PermissionSet grantable = guild_settings_grantable(owner->access);
    for (u32 group = 0; group < static_cast<u32>(PermissionGroup::count); ++group) {
        bool first = true;
        for (u32 i = 0; i < entries.count; ++i) {
            const auto& entry = entries[i];
            if (static_cast<u32>(entry.group) != group || !permission_grid_applies(entry, permission_scope_guild))
                continue;
            if (first) {
                push(category(control_permission_category + group,
                              permission_group_label(static_cast<PermissionGroup>(group))));
                first = false;
            }
            const bool on = (d.permissions & entry.bit) != 0;
            push({control_permission + i, entry.label, entry.description, UiSettingsControlKind::toggle,
                  writable && (on || (grantable & entry.bit) == entry.bit), on, {}, 0, {}, nullptr});
        }
    }
    if (r && guild_settings_role_deletable(owner->access, *r)) {
        push(category(control_delete_category, utf8_literal("Delete role")));
        push({control_delete, utf8_literal("Delete this role"),
              utf8_literal("Members lose the role and its permissions. This can't be undone."),
              UiSettingsControlKind::action, writable, false, {}, 0, utf8_literal("Delete role"), nullptr},
             {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    }
}

void open_role(u64 id) noexcept {
    const auto* r = role(id);
    if (!r || guard_dirty())
        return;
    release_picked(&owner->role_icon);
    load_role_editor(*r);
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
// The role list's menu: Copy Role ID (developer mode) and Delete Role.
enum RoleAction : u32 {
    role_action_copy_id = 1,
    role_action_delete,
};
void confirm_delete_role(u64 role_id) noexcept {
    const auto* r = role(role_id);
    if (!r || !guild_settings_role_deletable(owner->access, *r))
        return;
    static char context[256];
    Bank bank{context, sizeof(context), 0};
    bank.add(utf8_literal("Are you sure you want to delete the "));
    bank.add(chat_state_store_role_name(state(), *r));
    bank.add(utf8_literal(" role? This action cannot be undone."));
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
        if (id == control_create) {
            if (!p.order_dirty && !guard_dirty())
                queue(Op::role_create);
            repaint();
        } else if (id >= kRowBase && id < kRowBase + p.order_count)
            open_role(p.order[id - kRowBase]);
        return;
    }
    auto& d = p.role_draft;
    if (id == control_header) {
        if (activation.button || !activation.pointer)
            close_role();
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
        if (!guild_role_toggle(&d, p.access, entry.bit, activation.value.checked))
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
            const auto current = guild_role_draft_from(state(), *r);
            if (guild_role_draft_equal(current, p.role_base) || deadline_due(now, p.role_echo_until))
                p.role_echo = false;
        } else if (!mutation_pending()) {
            const auto current = guild_role_draft_from(state(), *r);
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
    const u64 role_id = p.order[id - kRowBase];
    const auto* r = role(role_id);
    if (!r)
        return;
    u32 count = 0;
    if (g_app.preferences.developer_mode)
        popup_row(count++, utf8_literal("Copy Role ID"), role_action_copy_id);
    if (page_allowed(Page::roles) && guild_settings_role_deletable(p.access, *r)) {
        popup_row(count, utf8_literal("Delete Role"), role_action_delete,
                  popup_item_danger | (count ? popup_item_separator_before : 0));
        ++count;
    }
    if (!count)
        return;
    p.menu = Menu::role;
    p.menu_item = role_id;
    settings_layer_open_choice(&p.layer, role_id, {p.popup_items, count}, activation.anchor, 240);
}

void choose(Menu menu, u32 action) noexcept {
    auto& p = *owner;
    if (menu != Menu::role || action < kMenuChoiceBase)
        return;
    if (action - kMenuChoiceBase == role_action_copy_id) {
        char digits[24];
        Bank bank{digits, sizeof(digits), 0};
        bank.number(p.menu_item);
        clipboard_write_utf8(g_app.window, bank.since(0));
    } else if (action - kMenuChoiceBase == role_action_delete && !busy())
        confirm_delete_role(p.menu_item);
}
} // namespace

const PageOps kRolesPage{load,      project,   activate,    choose,        dirty,  can_save,
                         next_save, reconcile, ensure_data, notice_action, invalid, context};

} // namespace maicord::app_detail::server_settings
