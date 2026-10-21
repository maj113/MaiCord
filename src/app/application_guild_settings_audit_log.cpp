#include "application_guild_settings_internal.hpp"
#include "application_account_presentation.hpp"
#include "application_settings.hpp"
#include "maicord/core/time.hpp"

namespace maicord::app_detail::server_settings {
namespace {

#include "application_audit_actions.inc"

// The actor's avatar from the response's users (hash or the default avatar),
// else the cached user's; an actor outside both keeps the initials.
UiSettingsAvatarMedia audit_avatar(u64 user_id) noexcept {
    for (u32 i = 0; i < owner->audit_user_count; ++i) {
        const auto& user = owner->audit_users[i];
        if (user.id == user_id)
            return application_listed_user_avatar_media(user_id, {user.avatar, user.avatar_bytes},
                                                        user.discriminator);
    }
    return application_user_avatar_media(user_id);
}

void project_audit(Bank& bank) noexcept {
    auto& p = *owner;
    p.audit_user_choice[0] = p.audit_filter_user ? bank.add(user_label(p.audit_filter_user)) : utf8_literal("All members");
    if (!p.audit_user_choice[0].bytes)
        p.audit_user_choice[0] = utf8_literal("Selected member");
    const auto* filter = p.audit_filter_action_set ? audit_action(p.audit_filter_action) : nullptr;
    p.audit_action_choice[0] = filter ? filter->verb : utf8_literal("All actions");
    push({control_filter_user, utf8_literal("Filter by member"), {}, UiSettingsControlKind::choice, editable(), false,
          {p.audit_user_choice, 1}, 0, {}, nullptr});
    push({control_filter_action, utf8_literal("Filter by action"), {}, UiSettingsControlKind::choice, editable(),
          false, {p.audit_action_choice, 1}, 0, {}, nullptr});
    if (p.audit_failed)
        push(notice(utf8_literal("Couldn't load the audit log"), p.status, utf8_literal("Try again")),
             {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    else if (p.failed && p.status.bytes)
        push(notice({}, p.status), {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    else if (!p.audit_complete && !p.audit_count)
        push(loading(utf8_literal("Loading the audit logÃ¢â‚¬Â¦")));
    push(category(control_list_category, utf8_literal("Recent actions")));
    if (p.audit_complete && !p.audit_count)
        push(notice({}, utf8_literal("No matching actions.")));
    const u64 now = clock_unix_milliseconds();
    for (u32 i = 0; i < p.audit_count; ++i) {
        const auto& entry = p.audit[i];
        const auto* action = audit_action(entry.action_type);
        const u32 start = bank.used;
        const auto actor = user_label(entry.user_id);
        bank.add(actor.bytes ? actor : utf8_literal("Someone"));
        bank.add(utf8_literal(" "));
        if (action) {
            bank.add(action->verb);
            Utf8View target{};
            if (action->target == 1)
                target = user_label(entry.target_id);
            else if (action->target == 2) {
                if (const auto* channel = chat_state_store_find_channel(state(), entry.target_id))
                    target = chat_state_store_channel_name(state(), *channel);
            } else if (action->target == 3) {
                if (const auto* r = role(entry.target_id))
                    target = chat_state_store_role_name(state(), *r);
            }
            if (target.bytes) {
                bank.add(utf8_literal(" "));
                bank.add(target);
            }
        } else {
            bank.add(utf8_literal("made a change (type "));
            bank.number(entry.action_type);
            bank.add(utf8_literal(")"));
        }
        const auto label = bank.since(start);
        const u32 detail = bank.used;
        relative_time(bank, (entry.id >> 22) + kSnowflakeEpochMilliseconds, now, false);
        if (entry.reason_bytes) {
            bank.add(utf8_literal(" Ã‚Â· "));
            bank.add({entry.reason, entry.reason_bytes});
        }
        const auto description = bank.since(detail);
        const u32 row =
            push({kRowBase + i, label, description, UiSettingsControlKind::action, true, false, {}, 0, {}, nullptr},
                 {UiSettingsVariant::standard,
                  static_cast<u8>(ui_settings_presentation_avatar | ui_settings_presentation_compact |
                                  ui_settings_presentation_accent),
                  kUiSettingsNoDefault});
        if (row < kControlCapacity)
            p.avatar_media[row] = audit_avatar(entry.user_id);
        p.row_accent[i] = person_color(entry.user_id);
    }
    // Older actions load when this row scrolls into view.
    if (p.audit_count && !p.audit_complete) {
        auto row = loading(utf8_literal("Loading older actionsÃ¢â‚¬Â¦"));
        row.id = control_more;
        push(row);
    }
}

void reset_audit() noexcept {
    owner->audit_count = 0;
    owner->audit_complete = false;
    owner->audit_requested = true;
    owner->scene.content_offset = 0;
    queue(Op::audit_page);
}
void open_audit_menu(Menu menu, UiRect anchor) noexcept {
    auto& p = *owner;
    u32 count = 0;
    const auto item = [&](Utf8View label, bool checked) noexcept {
        popup_row(count, label, count,
                  (checked ? popup_item_checked : 0) | (count == 1 ? popup_item_separator_before : 0));
        ++count;
    };
    if (menu == Menu::audit_user) {
        Bank bank{p.popup_text, sizeof(p.popup_text), 0};
        item(utf8_literal("All members"), !p.audit_filter_user);
        p.menu_users[0] = 0;
        for (u32 i = 0; i < p.audit_user_count && count < kPopupCapacity; ++i) {
            const auto& u = p.audit_users[i];
            p.menu_users[count] = u.id;
            item(bank.add(user_name(u)), u.id == p.audit_filter_user);
        }
    } else {
        item(utf8_literal("All actions"), !p.audit_filter_action_set);
        for (const u16 type : kAuditFilters) {
            const auto* action = audit_action(type);
            item(action ? action->verb : utf8_literal("Other"),
                 p.audit_filter_action_set && p.audit_filter_action == type);
        }
    }
    p.menu = menu;
    settings_layer_open_choice(&p.layer, p.guild_id, {p.popup_items, count}, anchor, 300);
}

void activate(const SettingsLayerActivation& activation) noexcept {
    const u64 id = activation.value.target.control_id;
    if (id == control_filter_user)
        open_audit_menu(Menu::audit_user, activation.anchor);
    else if (id == control_filter_action)
        open_audit_menu(Menu::audit_action, activation.anchor);
}

void choose(Menu menu, u32 action) noexcept {
    auto& p = *owner;
    if (menu == Menu::audit_user || menu == Menu::audit_action) {
        const u32 index = action - kMenuChoiceBase;
        if (action < kMenuChoiceBase || index >= kPopupCapacity || busy())
            return;
        if (menu == Menu::audit_user)
            p.audit_filter_user = p.menu_users[index];
        else {
            p.audit_filter_action_set = index > 0 && index <= sizeof(kAuditFilters) / sizeof(kAuditFilters[0]);
            p.audit_filter_action = p.audit_filter_action_set ? kAuditFilters[index - 1] : 0;
        }
        reset_audit();
    }
}

bool ensure_data() noexcept {
    if (owner->audit_requested)
        return false;
    owner->audit_requested = true;
    owner->audit_failed = false;
    queue(Op::audit_page);
    return true;
}

void notice_action() noexcept {
    if (!owner->audit_failed || busy() || !connected())
        return;
    owner->audit_requested = false;
    ensure_data();
}

} // namespace

bool audit_load_visible() noexcept {
    auto& p = *owner;
    if (p.page != Page::audit_log || !p.audit_count || p.audit_complete || busy() || !connected())
        return false;
    UiSettingsControlGeometry geometry{};
    if (!ui_settings_scene_control_geometry(p.scene, {UiSettingsTargetKind::control, page_key(Page::audit_log),
                                                      control_more},
                                            &geometry) ||
        !ui_rect_has_area(geometry.clip))
        return false;
    p.audit_requested = true;
    queue(Op::audit_page);
    return true;
}

const PageOps kAuditLogPage{nullptr, project_audit, activate,    choose,        nullptr, nullptr,
                            nullptr, nullptr,       ensure_data, notice_action, nullptr};

} // namespace maicord::app_detail::server_settings
