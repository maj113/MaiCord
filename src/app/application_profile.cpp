#include "application_scrolling.hpp"
#include "maicord/model/role_policy.hpp"
#include "application_profile.hpp"
#include "application_guild_card.hpp"
#include "application_friends.hpp"
#include "application_composer_text_input.hpp"
#include "application_composer_input.hpp"
#include "maicord/app/message_composer_controller.hpp"
#include "application_content_input.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_message_interaction.hpp"
#include "application_private_conversation.hpp"
#include "application_plugins.hpp"
#include "application_user_actions.hpp"
#include "application_profile_editor.hpp"
#include "application_composer.hpp"
#include "application_link_trust.hpp"
#include "text_edit_keys.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
template <u32 N> void assign(ProfileString<N>* to, Utf8View from) noexcept {
    to->bytes = static_cast<u16>(minimum(N, from.bytes));
    copy_bytes(to->data, from.data, to->bytes);
}
bool snapshot_message(MessageComposer* destination, Utf8View text) noexcept {
    if (!destination->text && !message_composer_initialize_owned(destination))
        return false;
    return message_composer_restore(destination, 0, text, text.bytes, text.bytes);
}
bool repaint(bool text = false) noexcept {
    if (text && !refresh_account_text(true))
        return false;
    return !g_app.window || application_request_frame() != FALSE;
}
MessageComposer& edit_model(ApplicationProfile& p) noexcept {
    return p.scene.note_editing ? p.note : p.message;
}
// Visible: the window is shown and not minimized (tests run without one).
bool window_shown() noexcept {
    return !g_app.window || (IsWindowVisible(g_app.window) && !IsIconic(g_app.window));
}
bool application_known(const ApplicationProfile& p, u64 id) noexcept {
    for (u32 i = 0; i < p.application_count; ++i)
        if (p.applications[i].id == id)
            return true;
    return false;
}
// Laid-out cards that would show an application icon not fetched yet.
bool missing_icons(const ApplicationProfile& p) noexcept {
    for (u32 i = 0; i < p.scene.missing_icon_count; ++i)
        if (!application_known(p, p.scene.missing_icons[i]))
            return true;
    return false;
}
void remember_application(ApplicationProfile& p, u64 id, Utf8View icon) noexcept {
    if (!id)
        return;
    ProfileApplication* slot = nullptr;
    for (u32 i = 0; i < p.application_count && !slot; ++i)
        if (p.applications[i].id == id)
            slot = &p.applications[i];
    if (!slot) {
        if (p.application_count < kProfileApplicationCapacity)
            slot = &p.applications[p.application_count++];
        else {
            slot = &p.applications[p.application_next];
            p.application_next = (p.application_next + 1) % kProfileApplicationCapacity;
        }
    }
    slot->id = id;
    slot->icon.bytes = static_cast<u16>(icon.bytes <= sizeof(slot->icon.data) ? icon.bytes : 0);
    copy_bytes(slot->icon.data, icon.data, slot->icon.bytes);
    p.scene.applications = {p.applications, p.application_count};
}
// One GET for the public applications (their icons) or a button's metadata.
bool submit_activity_request(ApplicationProfile& p, u64 now) noexcept {
    auto& host = g_app.account;
    if (!host.api.profile_get || !g_app.account_ui)
        return false;
    char ids[3 * 21];
    u32 used = 0, count = 0;
    const bool metadata = p.metadata_pending;
    if (!metadata) {
        for (u32 i = 0; i < p.scene.missing_icon_count && count < kUserActivityMaximum; ++i) {
            const u64 id = p.scene.missing_icons[i];
            if (application_known(p, id))
                continue;
            if (used)
                ids[used++] = ',';
            used += decimal_write(ids + used, id);
            p.requested_applications[count++] = id;
        }
        if (!count)
            return false;
    }
    auto& ui = *g_app.account_ui;
    if (++ui.message_history_request_id == 0)
        ++ui.message_history_request_id;
    const auto result =
        metadata ? host.api.profile_get(host.instance, p.metadata_user, p.metadata_application,
                                        ui.message_history_request_id, 6, p.metadata_session.view())
                 : host.api.profile_get(host.instance, p.data.user_id, 0, ui.message_history_request_id, 5, {ids, used});
    if (result.error == RestClientSubmitError::busy) {
        p.retry_at = now + 100;
        return true;
    }
    if (!result.ok()) {
        // Not retried: the card keeps its placeholder, the button does nothing.
        if (metadata)
            p.metadata_pending = false;
        else
            for (u32 i = 0; i < count; ++i)
                remember_application(p, p.requested_applications[i], {});
        return true;
    }
    p.requested_application_count = metadata ? 0 : count;
    p.submitted_request = metadata ? ProfileRequest::metadata : ProfileRequest::applications;
    p.retry_at = 0;
    g_app.details_request_owner = MessageHistoryRequestOwner::user_profile;
    g_app.details_request_id = ui.message_history_request_id;
    return true;
}
void visit_application(void* context, u64 id, Utf8View icon) noexcept {
    auto& p = *static_cast<ApplicationProfile*>(context);
    for (u32 i = 0; i < p.requested_application_count; ++i)
        if (p.requested_applications[i] == id)
            remember_application(p, id, icon);
}
bool activity_response(ApplicationProfile& p, const RestClientResponse& response, Utf8View body) noexcept {
    const bool metadata = p.submitted_request == ProfileRequest::metadata;
    p.submitted_request = ProfileRequest::profile;
    const bool ok = response.error == RestClientError::none && response.status_code == 200;
    if (metadata) {
        p.metadata_pending = false;
        char url[1024];
        const u32 bytes = ok ? user_activity_button_url(body, {g_app.account_ui->profile_decode.structurals, 16384},
                                                        p.metadata_button, {url, sizeof(url)})
                             : 0;
        // Only for the card still shown: a link from a closed card would surprise.
        if (bytes && p.scene.active && p.data.user_id == p.metadata_user)
            application_link_open({url, bytes}, false);
        return true;
    }
    if (ok)
        user_activity_applications_decode(body, {g_app.account_ui->profile_decode.structurals, 16384},
                                          visit_application, &p);
    // Unknown or failed IDs are not asked for again this session; a rate
    // limit tries again later.
    if (response.status_code == 429)
        p.retry_at = clock_monotonic_milliseconds() + 5000;
    else
        for (u32 i = 0; i < p.requested_application_count; ++i)
            if (!application_known(p, p.requested_applications[i]))
                remember_application(p, p.requested_applications[i], {});
    p.requested_application_count = 0;
    if (!p.scene.active)
        return true;
    return profile_scene_layout(&p.scene, application_viewport_dips(), true) && repaint(true);
}
// Copies the store's note for the card (the store may change underneath).
void load_note(ApplicationProfile& p) noexcept {
    auto& s = p.scene;
    const u64 user = p.data.user_id;
    s.note_visible = g_app.account_ui && user && user != g_app.account_ui->chat_state.self_user_id;
    if (!s.note_visible) {
        s.note_text = {};
        s.note_loading = false;
        return;
    }
    bool loading = false;
    const auto text = application_user_note(user, &loading);
    const u32 bytes = minimum<u32>(text.bytes, kUserNoteMaximumBytes);
    copy_bytes(p.note_display, text.data, bytes);
    s.note_text = {p.note_display, bytes};
    s.note_loading = loading && !bytes;
}
void sync_note_edit(ApplicationProfile& p) noexcept {
    auto& e = p.scene.edit;
    const auto& model = edit_model(p);
    e.text = message_composer_text(model);
    e.anchor = model.anchor;
    e.caret = model.caret;
    e.focused = model.focused;
    e.caret_visible = e.focused;
}
bool begin_note(ApplicationProfile& p) noexcept {
    if (!p.scene.note_visible || p.scene.note_loading)
        return true;
    if (!p.note.text && !message_composer_initialize_owned(&p.note, kUserNoteMaximumBytes))
        return false;
    const auto text = p.scene.note_text;
    if (!message_composer_restore(&p.note, 0, text, text.bytes, text.bytes))
        return false;
    p.note_user = p.data.user_id;
    p.note_text_revision = p.note.text_revision;
    p.message.focused = false;
    p.note.focused = true;
    p.scene.note_editing = true;
    sync_note_edit(p);
    profile_scene_layout(&p.scene, application_viewport_dips(), true);
    return application_profile_refresh_message();
}
// Leaving the field saves at once (the store skips unchanged notes).
void end_note(ApplicationProfile& p) noexcept {
    if (!p.scene.note_editing)
        return;
    p.scene.note_editing = false;
    p.note.focused = false;
    if (p.note_user)
        application_user_note_flush(p.note_user);
    load_note(p);
    sync_note_edit(p);
    p.message_dragging = false;
    profile_scene_layout(&p.scene, application_viewport_dips(), true);
}
constexpr u32 kMenuBase = 0x50460000u; // "PF"
enum : u32 {
    menu_edit_main = kMenuBase + 1,
    menu_edit_guild,
    menu_remove_friend,
    menu_cancel_request,
    menu_accept_request,
    menu_role_first = kMenuBase + 0x100,
    menu_last = kMenuBase + 0x1FF
};
u8 relation_of(u64 user) noexcept {
    u8 relation = 0;
    return application_friends_user_relation(user, &relation) ? relation : 0;
}
UiRect part_rectangle(const ApplicationProfile& p, UiWidgetHandle target) noexcept {
    const auto* node = ui_widget_manager_layout_node(p.scene.widgets, target);
    return node ? node->rectangle : p.scene.rectangle;
}
bool open_menu(ApplicationProfile& p, u32 count, UiRect anchor, float width) noexcept {
    if (!count || g_app.popup_scene.active || g_app.text_form)
        return true;
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, p.data.user_id, {p.menu_items, count}, anchor,
                          application_viewport_dips(), width)
             .ok())
        return false;
    p.menu_user = p.data.user_id;
    p.menu_guild = p.data.guild_id;
    set_popup_owner(PopupOwnerKind::profile);
    return refresh_popup_text();
}
PopupItem menu_row(Utf8View label, u32 action, Utf8View detail = {}, u16 flags = popup_item_enabled) noexcept {
    PopupItem item{};
    item.label = label;
    item.action = action;
    item.flags = flags;
    item.detail = detail;
    item.presentation.secondary_line = detail.bytes != 0;
    return item;
}
// Self in a server: "Edit Per-server Profile" / "Edit Main Profile".
bool open_edit_menu(ApplicationProfile& p, UiRect anchor) noexcept {
    p.menu_items[0] = menu_row(utf8_literal("Edit Per-server Profile"), menu_edit_guild,
                               utf8_literal("Visible only to those in this server"));
    p.menu_items[1] = menu_row(utf8_literal("Edit Main Profile"), menu_edit_main,
                               utf8_literal("Default profile and visible to everyone"));
    return open_menu(p, 2, anchor, 260);
}
// Friend Actions for the friend / pending header button.
bool open_friend_menu(ApplicationProfile& p, UiRect anchor) noexcept {
    const u8 relation = relation_of(p.data.user_id);
    u32 count = 0;
    if (relation == 1)
        p.menu_items[count++] = menu_row(utf8_literal("Remove Friend"), menu_remove_friend, {},
                                         popup_item_enabled | popup_item_danger);
    else if (relation == 2)
        p.menu_items[count++] = menu_row(utf8_literal("Accept Friend Request"), menu_accept_request);
    else if (relation == 3)
        p.menu_items[count++] = menu_row(utf8_literal("Cancel Friend Request"), menu_cancel_request, {},
                                         popup_item_enabled | popup_item_danger);
    return open_menu(p, count, anchor, 220);
}
// The role picker: assignable roles the member does not hold, in display order.
bool open_role_menu(ApplicationProfile& p, UiRect anchor) noexcept {
    if (!g_app.account_ui || p.mutation != ProfileRoleChange::none)
        return true;
    const auto& state = g_app.account_ui->chat_state;
    const u64 guild = p.data.guild_id;
    u32 begin = 0;
    while (begin < state.role_count && state.roles[begin].guild_id < guild)
        ++begin;
    u32 end = begin;
    while (end < state.role_count && state.roles[end].guild_id == guild)
        ++end;
    const ConstSlice<RoleRecord> roles{state.roles ? state.roles + begin : nullptr, end - begin};
    const auto policy = role_edit_policy(state, guild);
    u16 order[8192];
    const bool ordered = roles.count && role_display_order(roles, {order, 8192}, role_before);
    u32 count = 0;
    const RoleRecord* previous = nullptr;
    for (u32 i = 0; i < roles.count && count < 100; ++i) {
        const auto* role = ordered ? &roles[order[i]] : role_display_next(guild, roles, previous);
        if (!role)
            break;
        previous = role;
        bool held = false;
        for (u32 k = 0; k < p.data.role_count; ++k)
            held |= p.data.roles[k] == role->id;
        if (held || !role_editable(policy, *role))
            continue;
        auto item = menu_row(chat_state_store_role_name(state, *role), menu_role_first + count);
        item.presentation = ui_menu_row_presentation(UiIcon::mention);
        if (role->color) {
            item.media_flags = kPopupItemMediaIconTint;
            item.media_variant = role->color & 0xFFFFFFu;
        }
        p.menu_roles[count] = role->id;
        p.menu_items[count++] = item;
    }
    p.menu_role_count = count;
    if (!count) {
        p.menu_items[0] = menu_row(utf8_literal("No assignable roles"), menu_last, {}, 0);
        count = 1;
    }
    return open_menu(p, count, anchor, 240);
}
bool mutate_role(ApplicationProfile& p, u64 role, bool add) noexcept {
    if (p.mutation != ProfileRoleChange::none || !g_app.account_ui ||
        !role_editable(g_app.account_ui->chat_state, p.data.guild_id, role))
        return true;
    p.mutation = add ? ProfileRoleChange::add : ProfileRoleChange::remove;
    p.mutation_role = role;
    p.pending = true;
    p.retry_at = 0;
    p.scene.roles_busy = true;
    p.scene.phase = ProfilePhase::loading;
    return profile_scene_layout(&p.scene, application_viewport_dips(), true) && repaint(true);
}
// The popout becomes the full profile (UserProfileModalV2) on a tab.
bool show_modal(ApplicationProfile& p, u8 tab, bool note = false) noexcept {
    auto& s = p.scene;
    if (s.presentation != ProfilePresentation::modal) {
        p.message.focused = false;
        p.message_dragging = false;
        s.presentation = ProfilePresentation::modal;
        s.scroll = 0;
        s.hovered = false;
        s.hover_changed_ms = 0;
        // A new surface: its entrance replays.
        ++s.opens;
    }
    s.tab = tab;
    s.secondary_scroll = 0;
    s.scroll_to_note = note;
    if (!profile_scene_layout(&s, application_viewport_dips(), true))
        return false;
    if (note && !s.note_editing && !begin_note(p))
        return false;
    return repaint(true);
}
bool activate(ProfileAction action, UiWidgetHandle target = {}) noexcept {
    auto* p = g_app.profile;
    if (!p)
        return false;
    auto& s = p->scene;
    const auto* part = profile_scene_part(s, target);
    switch (action) {
    case ProfileAction::note:
        return s.note_editing || begin_note(*p);
    case ProfileAction::open_note:
        return show_modal(*p, 0, true);
    case ProfileAction::expand:
    case ProfileAction::view_bio:
    case ProfileAction::card:
    case ProfileAction::activity:
        return show_modal(*p, 0);
    case ProfileAction::friends:
        return show_modal(*p, 1);
    case ProfileAction::servers:
        return show_modal(*p, s.self_id == p->data.user_id || p->data.user_appearance.bot ? 0 : 2);
    case ProfileAction::roles_toggle:
        s.roles_expanded = !s.roles_expanded;
        break;
    case ProfileAction::choose_roles:
        return open_role_menu(*p, part_rectangle(*p, target));
    case ProfileAction::remove_role:
        return !part || mutate_role(*p, part->target, false);
    case ProfileAction::add_friend:
        if (!application_friends_user_action(p->data.user_id, ApplicationFriendsUserAction::add))
            log_u64(LogLevel::warning, "profile", "add_friend_refused", p->data.user_id);
        s.relation = relation_of(p->data.user_id);
        break;
    case ProfileAction::accept_friend:
        if (!application_friends_user_action(p->data.user_id, ApplicationFriendsUserAction::accept))
            log_u64(LogLevel::warning, "profile", "accept_friend_refused", p->data.user_id);
        s.relation = relation_of(p->data.user_id);
        break;
    case ProfileAction::relationship:
        return open_friend_menu(*p, part_rectangle(*p, target));
    case ProfileAction::guild_tag:
        return !part || !part->target || application_guild_card_open(part->target, part_rectangle(*p, target)) ||
               true;
    case ProfileAction::more:
        if (g_app.popup_scene.active || g_app.text_form)
            return true;
        return application_user_menu_open(p->data.user_id, part_rectangle(*p, target), p->data.guild_id,
                                          UserMenuSource::profile);
    case ProfileAction::edit:
        // In a server: per-server or main profile; elsewhere the main editor.
        if (p->data.guild_id && p->data.joined_at.bytes)
            return open_edit_menu(*p, part_rectangle(*p, target));
        application_profile_close();
        return application_profile_editor_open(ProfileEditorTab::main) || true;
    case ProfileAction::plugin_slot: {
        if (!part || part->media != ProfileMediaKind::plugin_slot)
            return false;
        UiPluginSlotNode nodes[8]{};
        const u64 user = p->data.user_id;
        const u32 count = ui_plugin_slots(kUiPluginSlotProfileSection, 0, 0, user, {nodes, 8});
        if (part->target >= count)
            return true;
        const auto& node = nodes[part->target];
        if (node.kind == ui_plugin_node_link) {
            if (node.value.bytes)
                application_open_content_url(node.value);
            return true;
        }
        if (node.action_id)
            application_plugins_slot_activate(node.owner_id, {kUiPluginSlotProfileSection, 0, 0, user, 0},
                                              node.action_id, part_rectangle(*p, target));
        return true;
    }
    case ProfileAction::open_friend:
    case ProfileAction::open_server: {
        if (!part)
            return false;
        const bool user = action == ProfileAction::open_friend;
        const u32 count = user ? p->data.mutual_friends_count : p->data.mutual_guild_count;
        if (part->source >= count)
            return false;
        const auto id = user ? p->data.mutual_friends[part->source].id : p->data.mutual_guilds[part->source].id;
        if (user) {
            // The official client opens that user's full profile.
            if (!application_profile_open(id, s.rectangle))
                return false;
            return application_profile_expand(id) || true;
        }
        application_profile_close();
        return select_content_guild(id);
    }
    case ProfileAction::activity_button: {
        // The button's URL lives in the activity's metadata, fetched on click.
        const auto* set = s.activity_set;
        if (!part || !set || part->source >= set->count || part->target > 1 || p->metadata_pending)
            return true;
        const auto session = user_activity_text(set->items[part->source], UserActivityField::session);
        if (!session.bytes || session.bytes > sizeof(p->metadata_session.data))
            return true;
        copy_bytes(p->metadata_session.data, session.data, session.bytes);
        p->metadata_session.bytes = static_cast<u16>(session.bytes);
        p->metadata_button = static_cast<u8>(part->target);
        p->metadata_user = p->data.user_id;
        p->metadata_application = set->items[part->source].application_id;
        p->metadata_pending = true;
        p->retry_at = 0;
        return true;
    }
    case ProfileAction::badge:
        // Through the link rules: Discord's own hosts open at once, others warn.
        if (part && part->source < p->data.badge_count && p->data.badges[part->source].link.bytes)
            application_open_content_url(p->data.badges[part->source].link.view());
        return true;
    case ProfileAction::activity_watch: {
        // The stream page opens like the official client's, without a warning.
        const auto* set = s.activity_set;
        if (part && set && part->source < set->count)
            application_link_open(user_activity_text(set->items[part->source], UserActivityField::url), true);
        return true;
    }
    case ProfileAction::close:
        return application_profile_close();
    case ProfileAction::message: {
        const auto id = p->data.user_id;
        application_profile_close();
        return queue_private_conversation(PrivateConversationOperation::open, id);
    }
    case ProfileAction::retry:
        p->pending = true;
        p->retry_at = 0;
        s.phase = ProfilePhase::loading;
        break;
    default:
        return true;
    }
    return profile_scene_layout(&s, application_viewport_dips(), true) && repaint(true);
}
} // namespace
bool application_profile_expand(u64 user) noexcept {
    auto* p = g_app.profile;
    if (!p || !p->scene.active || p->data.user_id != user)
        return false;
    return show_modal(*p, 0);
}
bool application_profile_menu_action(u32 action) noexcept {
    return action > kMenuBase && action <= menu_last && g_app.popup_owner_kind == PopupOwnerKind::profile;
}
bool application_profile_menu_activate(u32 action) noexcept {
    auto* p = g_app.profile;
    const u64 user = p ? p->menu_user : 0, guild = p ? p->menu_guild : 0;
    dismiss_popup();
    if (!p || !p->scene.active || p->data.user_id != user)
        return true;
    if (action == menu_edit_main || action == menu_edit_guild) {
        application_profile_close();
        return application_profile_editor_open(action == menu_edit_main ? ProfileEditorTab::main
                                                                        : ProfileEditorTab::guild,
                                               action == menu_edit_main ? 0 : guild) ||
               true;
    }
    if (action == menu_remove_friend || action == menu_cancel_request || action == menu_accept_request) {
        const auto intent = action == menu_remove_friend    ? ApplicationFriendsUserAction::remove
                            : action == menu_cancel_request ? ApplicationFriendsUserAction::cancel
                                                            : ApplicationFriendsUserAction::accept;
        if (!application_friends_user_action(user, intent))
            log_u64(LogLevel::warning, "profile", "relationship_action_refused", static_cast<u64>(intent));
        p->scene.relation = relation_of(user);
        return profile_scene_layout(&p->scene, application_viewport_dips(), true) && repaint(true);
    }
    if (action >= menu_role_first && action - menu_role_first < p->menu_role_count)
        return mutate_role(*p, p->menu_roles[action - menu_role_first], true);
    return true;
}
bool application_profile_open(u64 user, UiRect anchor, ProfileAnchor kind) noexcept {
    if (!g_app.account_ui || !user)
        return false;
    application_composer_text_input_suspend();
    end_message_composer_pointer_selection();
    end_message_text_selection();
    if (!g_app.profile)
        g_app.profile = static_cast<ApplicationProfile*>(
            VirtualAlloc(nullptr, sizeof(ApplicationProfile), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* p = g_app.profile;
    if (!p)
        return false;
    if (!p->message.text && !message_composer_initialize_owned(&p->message))
        return false;
    if (p->message_user != user)
        message_composer_reset(&p->message, 0, false);
    p->message_user = user;
    if (!p->message_pending && p->pending_recipient == user && p->pending_message.bytes) {
        if (snapshot_message(&p->message, message_composer_text(p->pending_message)))
            message_composer_clear(&p->pending_message);
    }
    p->message.focused = false;
    p->message_blink_at = 0;
    if (p->scene.note_editing)
        end_note(*p);
    p->scene.note_editing = false;
    p->scene.edit = {
        message_composer_text(p->message), p->message.anchor, p->message.caret, false, false, false};
    p->scene.message_failed = false;
    g_app.message_composer.focused = false;
    application_composer_text_input_suspend();
    p->anchor_kind = kind;
    // Message rows open right, the member list left (16 apart), the account panel above.
    p->scene.placement = kind == ProfileAnchor::account  ? ProfilePlacement::above
                         : kind == ProfileAnchor::member ? ProfilePlacement::left
                                                         : ProfilePlacement::right;
    p->scene.presentation = ProfilePresentation::popout;
    p->scene.relation = relation_of(user);
    p->scene.scroll_to_note = false;
    p->scene.opened_ms = clock_monotonic_milliseconds();
    const auto& state = g_app.account_ui->chat_state;
    const u64 guild = state.selected_guild_id;
    const bool cached = p->data.user_id == user && p->data.guild_id == guild && p->loaded_at &&
                        clock_monotonic_milliseconds() - p->loaded_at < 60000;
    dismiss_popup();
    if (!cached) {
        p->mutation = ProfileRoleChange::none;
        p->mutation_role = 0;
        p->scene.roles_busy = false;
        p->effect = {};
        p->effect_pending = false;
        p->data = {};
        p->data.user_id = user;
        p->data.guild_id = guild;
        p->loaded_at = 0;
        const auto* u = user_store_find(g_app.account_ui->user_store, user);
        if (u) {
            assign(&p->data.display_name, user_store_name(g_app.account_ui->user_store, *u));
            assign(&p->data.username, user_store_username(g_app.account_ui->user_store, *u));
            assign(&p->data.avatar, user_store_avatar_hash(*u));
        }
        const auto* member =
            guild_member_store_find(g_app.account_ui->guild_member_store, guild, user);
        p->scene.presence = member ? static_cast<UiPresence>(member->status) : UiPresence::unknown;
        // Outside a server (DMs, friends) a friend's presence still shows.
        const auto* relationships = application_friends_relationships();
        const auto* relation = p->scene.presence == UiPresence::unknown && relationships
                                   ? relationship_store_find(*relationships, user)
                                   : nullptr;
        if (relation && relation->type == RelationshipType::friend_)
            p->scene.presence = application_friends_presence(relation->presence, UiPresence::offline);
        p->scene.presence_mobile =
            ui_presence_mobile(p->scene.presence, user_store_client_status(g_app.account_ui->user_store, user));
        if (member)
            assign(&p->data.nickname, guild_member_store_nickname(*member));
        if (const auto* a = user_appearance_find(g_app.account_ui->user_store.appearance, user))
            user_appearance_expand(*a, &p->data.user_appearance);
        if (const auto* a =
                user_appearance_find(g_app.account_ui->user_store.appearance, user, guild))
            user_appearance_expand(*a, &p->data.guild_appearance);
        p->scene.phase = ProfilePhase::loading;
        p->pending = true;
        p->retry_at = 0;
    } else
        p->scene.phase = ProfilePhase::ready;
    p->scene.effect = &p->effect;
    p->scene.effect_started_ms = 0;
    load_note(*p);
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    p->scene.reduced_motion = !animations || g_app.preferences.reduced_motion;
    p->scene.measure = &g_app.account_ui->text_measure_cache;
    p->scene.applications = {p->applications, p->application_count};
    if (!profile_scene_open(&p->scene, &p->data, &state, &g_app.account_ui->user_store,
                            state.self_user_id, anchor, application_viewport_dips()))
        return false;
    g_app.renderer.profile_scene = &p->scene;
    return repaint(true);
}
bool application_profile_open_edit(u64 user, UiRect, ProfileAnchor) noexcept {
    if (!g_app.account_ui || user != g_app.account_ui->chat_state.self_user_id || !user)
        return false;
    application_profile_close();
    return application_profile_editor_open(ProfileEditorTab::main);
}
bool application_profile_close() noexcept {
    if (!g_app.profile || !g_app.profile->scene.active)
        return true;
    application_composer_text_input_suspend();
    application_surface_motion_retire(RendererLayerSurface::profile);
    auto& s = g_app.profile->scene;
    end_note(*g_app.profile);
    s.active = false;
    g_app.profile->message.focused = false;
    g_app.profile->message_blink_at = 0;
    g_app.profile->message_dragging = false;
    g_app.profile->pending = false;
    if (g_app.profile->mutation != ProfileRoleChange::none) {
        g_app.profile->loaded_at = 0;
        g_app.profile->mutation = ProfileRoleChange::none;
        g_app.profile->mutation_role = 0;
        s.roles_busy = false;
    }
    ui_button_pointer(&s.widgets, UiButtonPointerEvent::cancel, {});
    ui_button_keyboard_cancel(&s.keyboard);
    if (GetCapture() == g_app.window)
        ReleaseCapture();
    ++s.revision;
    return repaint(true);
}
void application_profile_release() noexcept {
    application_composer_text_input_suspend();
    application_surface_motion_retire(RendererLayerSurface::profile);
    g_app.renderer.profile_scene = nullptr;
    if (g_app.profile) {
        message_composer_release(&g_app.profile->pending_message);
        message_composer_release(&g_app.profile->message);
        message_composer_release(&g_app.profile->note);
        profile_scene_release(&g_app.profile->scene);
        VirtualFree(g_app.profile, 0, MEM_RELEASE);
        g_app.profile = nullptr;
    }
}
void application_profile_layout(UiRect viewport) noexcept {
    auto* p = g_app.profile;
    if (!p || !p->scene.active)
        return;
    auto& s = p->scene;
    auto anchor = s.anchor;
    if (p->anchor_kind == ProfileAnchor::account) {
        anchor = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::channel_footer);
    } else if (p->anchor_kind == ProfileAnchor::member) {
        const auto& members = g_app.member_list_scene;
        if (ui_rect_has_area(members.viewport)) {
            anchor.left = members.viewport.left;
            anchor.right = members.viewport.right;
        }
        for (u32 i = 0; i < members.count; ++i) {
            if (members.rows[i].user_id != p->data.user_id)
                continue;
            const auto* row = member_list_scene_layout_node(members, members.rows[i].widget);
            if (row)
                anchor = row->rectangle;
            break;
        }
    } else if (!ui_rect_equal(s.viewport, viewport) && ui_rect_width(s.viewport) > 0) {
        const float dx = (viewport.right - s.viewport.right) * (s.anchor.left - s.viewport.left) /
                         ui_rect_width(s.viewport);
        anchor.left += dx;
        anchor.right += dx;
    }
    // Friend changes (add, accept, remove) show in the header buttons.
    const u8 relation = relation_of(p->data.user_id);
    const bool moved = !ui_rect_equal(s.anchor, anchor) || relation != s.relation;
    s.anchor = anchor;
    s.relation = relation;
    profile_scene_layout(&s, viewport, moved);
}
bool application_profile_refresh_message() noexcept {
    auto* p = g_app.profile;
    if (!p || !p->scene.active)
        return false;
    auto& e = p->scene.edit;
    sync_note_edit(*p);
    if (p->scene.note_editing && p->note.text_revision != p->note_text_revision) {
        p->note_text_revision = p->note.text_revision;
        application_user_note_edit(p->note_user, message_composer_text(p->note));
    }
    const u32 blink = GetCaretBlinkTime();
    p->message_blink_at =
        e.focused && blink && blink != INFINITE ? clock_monotonic_milliseconds() + blink : 0;
    ++p->scene.revision;
    return repaint(true);
}
void application_profile_tick(u64 now) noexcept {
    auto* p = g_app.profile;
    // The popout's entrance slide re-lays out until it settles.
    if (p && p->scene.active && p->scene.intro_offset != profile_scene_intro_offset(p->scene, now)) {
        profile_scene_layout(&p->scene, p->scene.viewport, true);
        repaint(true);
    }
    // Rich presence timers advance each second while the window shows them.
    if (p && p->scene.active && p->scene.activity_timed && window_shown() &&
        profile_scene_tick(&p->scene, clock_unix_milliseconds()))
        repaint(true);
    if (!p || !p->scene.active || !p->message_blink_at || now < p->message_blink_at)
        return;
    p->scene.edit.caret_visible = !p->scene.edit.caret_visible;
    p->message_blink_at = now + GetCaretBlinkTime();
    repaint();
}
bool application_profile_conversation_ready() noexcept {
    auto* p = g_app.profile;
    if (!p || !p->message_pending || !g_app.account_ui)
        return true;
    auto& ui = *g_app.account_ui;
    const u64 channel = private_conversation_cached(ui.chat_state, p->pending_recipient);
    if (ui.private_conversation.phase == PrivateConversationPhase::failed &&
        ui.private_conversation.operation == PrivateConversationOperation::open &&
        ui.private_conversation.target_id == p->pending_recipient) {
        p->message_pending = false;
        p->scene.message_failed = p->message_user == p->pending_recipient;
    } else if (!channel)
        return true;
    else {
        p->message_pending = false;
        // A navigation change while opening the DM must not send to another channel.
        if (channel != ui.active_channel_id)
            p->scene.message_failed = p->message_user == p->pending_recipient;
        else {
            p->pending_message.channel_id = channel;
            const auto queued = message_composer_controller_queue_send(
                &ui.message_composer_controller, true, ui.chat_state.self_user_id,
                clock_unix_milliseconds(), clock_monotonic_milliseconds(), 0, {}, {}, &p->pending_message);
            if (queued.ok()) {
                if (p->message_user == p->pending_recipient &&
                    p->message.text_revision == p->pending_message_revision) {
                    message_composer_clear(&p->message);
                    application_profile_close();
                }
                return true;
            }
            p->scene.message_failed = p->message_user == p->pending_recipient;
        }
    }
    if (!p->scene.active || p->message_user != p->pending_recipient)
        return true;
    profile_scene_layout(&p->scene, application_viewport_dips(), true);
    return application_profile_refresh_message();
}
bool application_profile_character(u32 character) noexcept {
    auto* p = g_app.profile;
    if (!p || !p->scene.active)
        return false;
    auto& model = edit_model(*p);
    if (!p->scene.edit.active || !model.focused || (p->message_pending && !p->scene.note_editing) || character < 32 ||
        character == 127 || character > 0xFFFF ||
        ((GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)))
        return true;
    if (message_composer_codepoints(model) >= (p->scene.note_editing ? kUserNoteMaximumCodepoints : ~0u))
        return true;
    if (message_composer_insert_utf16(&model, static_cast<u16>(character),
                                      clock_monotonic_milliseconds()) == MessageComposerResult::changed)
        application_profile_refresh_message();
    return true;
}
bool application_profile_pointer(UiButtonPointerEvent event, UiPoint point) noexcept {
    auto* p = g_app.profile;
    if (!p || !p->scene.active || g_app.popup_scene.active || g_app.text_form)
        return false;
    if (event == UiButtonPointerEvent::move)
        update_scrollbar_hover(point.x, point.y);
    auto& s = p->scene;
    if (event == UiButtonPointerEvent::press)
        s.focus_visible = false;
    // Card hover shows the note button and dims the profile effect.
    const bool over = ui_rect_contains(s.presentation == ProfilePresentation::modal ? s.card : s.rectangle, point.x,
                                       point.y) &&
                      event != UiButtonPointerEvent::cancel;
    if (over != s.hovered) {
        s.hovered = over;
        s.hover_changed_ms = clock_monotonic_milliseconds();
        ++s.revision;
        repaint();
    }
    if (event == UiButtonPointerEvent::press && !ui_rect_contains(s.rectangle, point.x, point.y)) {
        application_profile_close();
        return true;
    }
    MessageTextSemantic bio_value{};
    const MessageTextSemantic* bio_hit = nullptr;
    const auto* text = g_app.renderer.content_text;
    if (text)
        for (u32 i = text->content_hit_region_count; i > 0; --i) {
            const auto& r = text->content_hit_regions[i - 1];
            if (point.x < r.left || point.x >= r.right || point.y < r.top || point.y >= r.bottom ||
                r.semantic_index >= text->content_semantic_count)
                continue;
            const auto& semantic = text->content_semantics[r.semantic_index];
            if (!semantic.message_id && (semantic.kind == MessageMarkdownSemanticKind::spoiler ||
                                         semantic.kind == MessageMarkdownSemanticKind::link)) {
                bio_value = semantic;
                bio_hit = &bio_value;
                break;
            }
        }
    if (event == UiButtonPointerEvent::press && bio_hit) {
        s.bio_pressed = bio_hit->source_offset + 1;
        SetCapture(g_app.window);
        return true;
    }
    if (event == UiButtonPointerEvent::release && s.bio_pressed) {
        const u32 pressed = s.bio_pressed;
        s.bio_pressed = 0;
        if (GetCapture() == g_app.window)
            ReleaseCapture();
        if (bio_hit && bio_hit->source_offset + 1 == pressed) {
            if (bio_hit->kind == MessageMarkdownSemanticKind::spoiler && s.bio_revealed_count < 64) {
                s.bio_revealed[s.bio_revealed_count++] = bio_hit->source_offset;
                profile_scene_layout(&s, application_viewport_dips(), true);
                repaint(true);
            } else if (bio_hit->kind == MessageMarkdownSemanticKind::link) {
                const auto source =
                    s.profile->guild_bio.bytes ? s.profile->guild_bio.view() : s.profile->bio.view();
                if (bio_hit->source_offset + bio_hit->source_bytes <= source.bytes)
                    application_open_content_url(
                        {source.data + bio_hit->source_offset, bio_hit->source_bytes});
            }
        }
        return true;
    }
    if (event == UiButtonPointerEvent::cancel)
        s.bio_pressed = 0;
    const bool in_edit = s.edit.active && ui_rect_contains(s.edit_rectangle, point.x, point.y);
    if (event == UiButtonPointerEvent::cancel)
        p->message_dragging = false;
    if (s.edit.active && ((event == UiButtonPointerEvent::press && in_edit) || p->message_dragging)) {
        if (event == UiButtonPointerEvent::press) {
            edit_model(*p).focused = true;
            p->message_dragging = true;
            ui_widget_manager_focus(&s.widgets, s.message_field);
            ui_button_pointer(&s.widgets, UiButtonPointerEvent::cancel, {});
            SetFocus(g_app.window);
            SetCapture(g_app.window);
        }
        if (event == UiButtonPointerEvent::press || event == UiButtonPointerEvent::move) {
            auto& model = edit_model(*p);
            u32 position = 0, byte = model.caret;
            if (text_edit_layout_hit_test(g_app.renderer.profile_edit_layout, point.x, point.y,
                                            &position))
                utf8_byte_offset_for_utf16_position(message_composer_text(model), position, &byte);
            const bool extend = event != UiButtonPointerEvent::press || (GetKeyState(VK_SHIFT) & 0x8000);
            message_composer_set_selection(&model, extend ? model.anchor : byte, byte);
        }
        if (event == UiButtonPointerEvent::release || event == UiButtonPointerEvent::cancel) {
            p->message_dragging = false;
            if (GetCapture() == g_app.window)
                ReleaseCapture();
        }
        application_profile_refresh_message();
        return true;
    }
    if (event == UiButtonPointerEvent::press && p->scene.note_editing) {
        end_note(*p);
        application_profile_refresh_message();
    } else if (event == UiButtonPointerEvent::press && p->message.focused) {
        p->message.focused = false;
        application_profile_refresh_message();
    }
    const auto previous_hover = s.widgets.hovered;
    const auto result = ui_button_pointer_target(&s.widgets, event,
        profile_scene_hit_test(s, point.x, point.y));
    if (event == UiButtonPointerEvent::press && result.handled) {
        SetFocus(g_app.window);
        SetCapture(g_app.window);
    }
    if (event == UiButtonPointerEvent::release && GetCapture() == g_app.window)
        ReleaseCapture();
    if (result.changed) {
        bool text_changed = false;
        if (previous_hover.slot != s.widgets.hovered.slot ||
            previous_hover.generation != s.widgets.hovered.generation) {
            for (u32 i = 0; i < s.part_count; ++i) {
                const auto& part = s.parts[i];
                if (part.look == ProfileLook::plain && part.text.bytes && part.action != ProfileAction::none)
                    text_changed |=
                        (part.widget.slot == previous_hover.slot && part.widget.generation == previous_hover.generation) ||
                        (part.widget.slot == s.widgets.hovered.slot && part.widget.generation == s.widgets.hovered.generation);
            }
        }
        if (text_changed) ++s.revision;
        repaint(text_changed);
    }
    if (result.activate)
        activate(profile_scene_action(s, result.target), result.target);
    return true;
}
bool application_profile_double_click(UiPoint point) noexcept {
    auto* p = g_app.profile;
    if (!p || !p->scene.active)
        return false;
    if (p->scene.edit.active && ui_rect_contains(p->scene.edit_rectangle, point.x, point.y)) {
        application_profile_pointer(UiButtonPointerEvent::press, point);
        auto& model = edit_model(*p);
        message_composer_select_word(&model, model.caret);
        // The press re-armed character dragging; a jittery move must not
        // collapse the word that was just selected.
        p->message_dragging = false;
        if (GetCapture() == g_app.window)
            ReleaseCapture();
        application_profile_refresh_message();
    }
    return true;
}
bool application_profile_key(u32 key, bool down, bool repeated) noexcept {
    auto* p = g_app.profile;
    if (!p || !p->scene.active)
        return false;
    auto& s = p->scene;
    if (s.note_editing && p->note.focused && (key == VK_RETURN || key == VK_ESCAPE)) {
        if (down && !repeated && !application_composer_text_input_composing()) {
            end_note(*p);
            application_profile_refresh_message();
        }
        return true;
    }
    if (s.edit.active && edit_model(*p).focused && key != VK_TAB && key != VK_ESCAPE) {
        if (!down || (p->message_pending && !s.note_editing))
            return true;
        auto* edit = &edit_model(*p);
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000);
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        MessageComposerResult result = MessageComposerResult::unchanged;
        if (key == VK_RETURN) {
            if (application_composer_text_input_composing())
                return true;
            if (!repeated &&
                message_composer_codepoints(*edit) <= kMessageComposerDefaultMaximumCodepoints &&
                edit->bytes) {
                if (!snapshot_message(&p->pending_message, message_composer_text(*edit)))
                    return true;
                p->pending_recipient = p->message_user;
                p->pending_message_revision = edit->text_revision;
                p->message_pending = true;
                if (!queue_private_conversation(PrivateConversationOperation::open, p->message_user)) {
                    p->message_pending = false;
                    s.message_failed = true;
                    profile_scene_layout(&s, s.viewport, true);
                    application_profile_refresh_message();
                } else
                    application_profile_conversation_ready();
            }
            return true;
        }
        if (control && (key == 'C' || key == 'X')) {
            const auto first = minimum(edit->anchor, edit->caret),
                       last = maximum(edit->anchor, edit->caret);
            if (first != last && clipboard_write_utf8(g_app.window, {edit->text + first, last - first}) &&
                key == 'X')
                result = message_composer_delete_forward(edit);
        } else if (control && key == 'V') {
            char text[kMessageComposerMaximumBytes];
            const auto read = clipboard_read_utf8(g_app.window, {text, sizeof(text)});
            if (read.ok())
                result = message_composer_insert_utf8(edit, {text, read.bytes});
        } else
            message_composer_edit_key(edit, key, control, shift, &result);
        if (result == MessageComposerResult::changed)
            application_profile_refresh_message();
        return true;
    }
    if (key == VK_RETURN || key == VK_SPACE) {
        const auto result = ui_button_keyboard_key(
            &s.keyboard, &s.widgets, key == VK_SPACE ? UiButtonKey::space : UiButtonKey::enter, down,
            repeated);
        if (result.activate)
            activate(profile_scene_action(s, result.target), result.target);
        repaint();
        return true;
    }
    if (!down || repeated)
        return true;
    if (key == VK_ESCAPE) {
        application_profile_close();
        return true;
    }
    if (key == VK_TAB) {
        s.focus_visible = true;
        ui_widget_manager_focus(&s.widgets,
                                ui_widget_manager_next_focus(s.widgets, s.widgets.focused,
                                                             (GetKeyState(VK_SHIFT) & 0x8000) != 0, true));
        edit_model(*p).focused = s.edit.active && s.widgets.focused.slot == s.message_field.slot &&
                             s.widgets.focused.generation == s.message_field.generation;
        application_profile_refresh_message();
    } else if (key == VK_DOWN || key == VK_UP) {
        queue_profile_scroll(false,
                             key == VK_DOWN ? -WHEEL_DELTA : WHEEL_DELTA);
        repaint(true);
    }
    return true;
}
bool application_profile_submit(u64 now) noexcept {
    auto* p = g_app.profile;
    if (!p || !p->scene.active || p->retry_at > now)
        return false;
    if (!p->pending && !p->effect_pending)
        return submit_activity_request(*p, now);
    auto& host = g_app.account;
    auto& ui = *g_app.account_ui;
    const bool roles = p->mutation != ProfileRoleChange::none;
    if (roles && !role_editable(ui.chat_state, p->data.guild_id, p->mutation_role)) {
        p->pending = false;
        p->mutation = ProfileRoleChange::none;
        p->mutation_role = 0;
        p->scene.roles_busy = false;
        p->scene.phase = ProfilePhase::failed;
        profile_scene_layout(&p->scene, p->scene.viewport, true);
        repaint(true);
        return true;
    }
    if (++ui.message_history_request_id == 0)
        ++ui.message_history_request_id;
    const bool effect = p->effect_pending && !p->pending;
    const auto effect_id =
        p->data.guild_effect_sku_id ? p->data.guild_effect_sku_id : p->data.effect_sku_id;
    RestClientSubmitResult result{RestClientSubmitError::invalid_input};
    if (roles && !effect) {
        MemberPatch patch{};
        patch.fields = member_patch_roles;
        patch.role_ids = {&p->mutation_role, 1};
        GuildAdminRequest request{};
        request.operation = p->mutation == ProfileRoleChange::add ? GuildAdminOperation::member_role_add
                                                                  : GuildAdminOperation::member_role_remove;
        request.guild_id = p->data.guild_id;
        request.target_id = p->data.user_id;
        request.member = &patch;
        if (host.api.guild_admin)
            result = host.api.guild_admin(host.instance, &request, ui.message_history_request_id);
    } else if (host.api.profile_get) {
        result = host.api.profile_get(host.instance, effect ? effect_id : p->data.user_id, p->data.guild_id,
                                      ui.message_history_request_id, effect ? 4 : 0, {});
    }
    if (result.error == RestClientSubmitError::busy) {
        p->retry_at = now + 100;
        return true;
    }
    if (!result.ok()) {
        p->pending = false;
        p->mutation = ProfileRoleChange::none;
        p->mutation_role = 0;
        p->scene.roles_busy = false;
        p->scene.phase = ProfilePhase::failed;
        profile_scene_layout(&p->scene, p->scene.viewport, true);
        repaint(true);
        return true;
    }
    p->requested_effect = effect ? effect_id : 0;
    if (effect)
        p->effect_pending = false;
    p->submitted_request = effect  ? ProfileRequest::effect
                           : roles ? ProfileRequest::roles
                                   : ProfileRequest::profile;
    p->requested_user = p->data.user_id;
    p->requested_guild = p->data.guild_id;
    p->pending = false;
    p->retry_at = 0;
    g_app.details_request_owner = MessageHistoryRequestOwner::user_profile;
    g_app.details_request_id = ui.message_history_request_id;
    return true;
}
bool application_profile_response(const RestClientResponse& response, Utf8View body) noexcept {
    auto* p = g_app.profile;
    if (p && (p->submitted_request == ProfileRequest::applications ||
              p->submitted_request == ProfileRequest::metadata))
        return activity_response(*p, response, body);
    if (!p || p->requested_user != p->data.user_id || p->requested_guild != p->data.guild_id)
        return true;
    if (p->submitted_request == ProfileRequest::effect) {
        p->submitted_request = ProfileRequest::profile;
        if (response.error == RestClientError::none && response.status_code == 200) {
            profile_effect_decode(body, {g_app.account_ui->profile_decode.structurals, 16384},
                                          p->requested_effect, &p->effect);
            p->scene.effect_started_ms = 0;
        }
        return repaint();
    }
    if (p->submitted_request == ProfileRequest::roles) {
        const bool saved = response.error == RestClientError::none && response.status_code >= 200 &&
                           response.status_code < 300;
        p->submitted_request = ProfileRequest::profile;
        p->scene.roles_busy = false;
        p->mutation = ProfileRoleChange::none;
        p->mutation_role = 0;
        if (saved) {
            p->pending = true;
            p->retry_at = 0;
            p->loaded_at = 0;
            p->scene.phase = ProfilePhase::loading;
        } else {
            p->scene.phase = ProfilePhase::failed;
            p->scene.http_status = response.status_code;
        }
        if (!p->scene.active)
            return true;
        return profile_scene_layout(&p->scene, application_viewport_dips(), true) && repaint(true);
    }
    bool parsed = false;
    if (response.error == RestClientError::none && response.status_code == 200) {
        parsed = g_app.account_ui->profile_decode.decode_into(body, p->requested_user,
                                                            p->requested_guild, p->data);
    }
    p->scene.phase = parsed ? ProfilePhase::ready : ProfilePhase::failed;
    p->scene.http_status = response.status_code;
    if (parsed) {
        p->effect_pending = (p->data.guild_effect_sku_id || p->data.effect_sku_id) != 0;
        p->loaded_at = clock_monotonic_milliseconds();
        auto& users = g_app.account_ui->user_store;
        user_store_upsert(&users, p->data.user_id, p->data.display_name.view(),
                                  p->data.username.view());
        user_store_set_avatar(&users, p->data.user_id, p->data.avatar.view());
        user_appearance_upsert(&users.appearance, p->data.user_appearance);
        if (p->data.guild_appearance.user_id)
            user_appearance_upsert(&users.appearance, p->data.guild_appearance);
        ++users.revision;
    }
    if (!p->scene.active)
        return true;
    return profile_scene_layout(&p->scene, application_viewport_dips(), true) && repaint(true);
}
MessageComposer* application_profile_edit_model() noexcept {
    return g_app.profile ? &edit_model(*g_app.profile) : nullptr;
}
bool application_profile_edit_focused() noexcept {
    auto* p = g_app.profile;
    return p && p->scene.active && p->scene.edit.active && edit_model(*p).focused &&
           (p->scene.note_editing || !p->message_pending);
}
void application_profile_note_changed() noexcept {
    auto* p = g_app.profile;
    if (!p || !p->scene.active || p->scene.note_editing)
        return;
    load_note(*p);
    profile_scene_layout(&p->scene, application_viewport_dips(), true);
    repaint(true);
}
u64 application_profile_next_wake() noexcept {
    const auto blink = g_app.profile && g_app.profile->scene.active ? g_app.profile->message_blink_at : 0;
    const auto network = g_app.details_request_owner == MessageHistoryRequestOwner::none &&
                                 g_app.profile && g_app.profile->scene.active &&
                                 (g_app.profile->pending || g_app.profile->effect_pending ||
                                  g_app.profile->metadata_pending || missing_icons(*g_app.profile))
                             ? maximum<u64>(1, g_app.profile->retry_at)
                             : 0;
    u64 wake = network && blink ? minimum(network, blink) : network ? network : blink;
    if (g_app.profile && g_app.profile->scene.active && g_app.profile->scene.intro_offset > 0) {
        const u64 frame = clock_monotonic_milliseconds() + kUiTransitionFrameMs;
        wake = wake ? minimum(wake, frame) : frame;
    }
    if (g_app.profile && g_app.profile->scene.active && g_app.profile->scene.activity_timed && window_shown()) {
        const u64 clock = clock_monotonic_milliseconds() + (1000 - clock_unix_milliseconds() % 1000);
        wake = wake ? minimum(wake, clock) : clock;
    }
    return wake;
}
} // namespace maicord::app_detail
