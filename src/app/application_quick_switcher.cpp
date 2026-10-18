#include "application_downloads.hpp"
#include "text_edit_keys.hpp"
#include "maicord/app/shortcut_registry.hpp"
#include "application_quick_switcher.hpp"
#include "application_cursor.hpp"
#include "maicord/ui/scene_cursor.hpp"
#include "application_voice.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_composer_text_input.hpp"
#include "application_content_input.hpp"
#include "application_friends.hpp"
#include "application_media_picker.hpp"
#include "application_search.hpp"
#include "application_pinned_messages.hpp"
#include "application_profile.hpp"
#include "application_private_conversation.hpp"
#include "application_private_channel.hpp"
#include "application_message_interaction.hpp"
#include "application_account_menu.hpp"
#include "application_surface_motion.hpp"
#include "application_theme_editor.hpp"
#include "application_video.hpp"
#include "maicord/ui/quick_switcher_scene.hpp"
#include "maicord/app/private_channel_actions.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/protocol/writer.hpp"

#include <Windows.h>

namespace maicord::app_detail {
struct ApplicationQuickSwitcher {
    QuickSwitcherController controller;
    MessageComposer edit;
    UiQuickSwitcherScene scene;
    UiQuickSwitcherSource source;
    UiQuickSwitcherContent content;
    UiQuickSwitcherTarget pressed;
    char query[kQuickSwitcherQueryMaximumBytes];
    u64 identity, blink_at;
    u32 content_revision;
    bool active, focused, caret_visible, captured, dragging;
    // People picker: zero starts a conversation, otherwise people join this channel.
    u64 picker_channel;
    u64 picked[kPrivateChannelRecipientMaximum];
    u32 picker_limit;
    u8 picked_count;
    bool picker;
    char hint[64], status[32];
};
namespace {
QuickSwitcherScope scope() noexcept {
    return {g_app.plugin_account_scope_serial, g_app.account.status.profile_id,
            g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0};
}
bool same_scope(QuickSwitcherScope a, QuickSwitcherScope b) noexcept {
    return a.account_serial == b.account_serial && a.profile_key == b.profile_key && a.self_user_id == b.self_user_id;
}
QuickSwitcherSources sources() noexcept {
    return g_app.account_ui ? QuickSwitcherSources{&g_app.account_ui->chat_state,
        &g_app.account_ui->user_store, application_friends_relationships()} : QuickSwitcherSources{};
}
bool available() noexcept {
    const auto s = scope();
    return !(application_downloads_active() || (g_app.renderer.message_forward_scene && g_app.renderer.message_forward_scene->active)) && g_app.renderer_ready && g_app.account_ui && g_app.account.instance && !g_app.account.stop_failed &&
        s.account_serial && s.profile_key && s.self_user_id && !g_app.login.visible &&
        !(g_app.renderer.text_form && g_app.renderer.text_form->scene && g_app.renderer.text_form->scene->active) &&
        !application_theme_editor_active() &&
        !(g_app.renderer.settings_scene && g_app.renderer.settings_scene->active) &&
        !g_app.image_viewer.scene.active && !g_app.reaction_viewer.active;
}
void recent_scope() noexcept {
    const auto now = scope();
    if (same_scope(g_app.quick_switcher_recent_scope, now)) return;
    g_app.quick_switcher_recent_scope = now;
    g_app.quick_switcher_recent_count = 0;
    SecureZeroMemory(g_app.quick_switcher_recent_channels, sizeof(g_app.quick_switcher_recent_channels));
}
UiQuickSwitcherKind ui_kind(QuickSwitcherKind kind) noexcept {
    switch (kind) {
    case QuickSwitcherKind::text_channel: return UiQuickSwitcherKind::text_channel;
    case QuickSwitcherKind::private_channel: return UiQuickSwitcherKind::private_channel;
    case QuickSwitcherKind::group_private: return UiQuickSwitcherKind::group_private;
    case QuickSwitcherKind::guild: return UiQuickSwitcherKind::guild;
    case QuickSwitcherKind::user: return UiQuickSwitcherKind::user;
    case QuickSwitcherKind::voice_channel: return UiQuickSwitcherKind::voice_channel;
    }
    return UiQuickSwitcherKind::text_channel;
}
bool source_current(void* opaque) noexcept {
    const auto* q = static_cast<const ApplicationQuickSwitcher*>(opaque);
    return q && q == g_app.quick_switcher && q->active && available() &&
        quick_switcher_current(q->controller, scope(), sources(), q->source.revision);
}
// A friend's status, as the DM list shows it; unknown for everyone else.
UiPresence friend_presence(u64 user, bool* mobile) noexcept {
    *mobile = false;
    const auto* relationships = application_friends_relationships();
    const auto* relation = relationships && user ? relationship_store_find(*relationships, user) : nullptr;
    if (!relation || relation->type != RelationshipType::friend_) return UiPresence::unknown;
    const auto presence = application_friends_presence(relation->presence, UiPresence::offline);
    *mobile = ui_presence_mobile(presence, user_store_client_status(g_app.account_ui->user_store, user));
    return presence;
}
bool resolve_row(void* opaque, u32 index, UiQuickSwitcherRow* out) noexcept {
    auto* q = static_cast<ApplicationQuickSwitcher*>(opaque);
    QuickSwitcherTarget target{};
    if (!out || !source_current(q) ||
        !quick_switcher_resolve(q->controller, scope(), sources(), index, q->source.revision, &target)) return false;
    const auto& row = q->controller.rows[index];
    *out = {target.id, target.guild_id, target.channel_id, target.user_id, ui_kind(target.kind),
        {row.label, row.label_bytes}, {row.detail, row.detail_bytes},
        row.recent ? utf8_literal("Recent") : Utf8View{}, {row.avatar_hash, row.avatar_hash_bytes}, true};
    if (target.kind == QuickSwitcherKind::user || target.kind == QuickSwitcherKind::private_channel)
        out->presence = friend_presence(target.user_id, &out->mobile);
    if (q->picker && q->picker_channel &&
        private_channel_has_member(g_app.account_ui->chat_state, q->picker_channel, target.user_id)) {
        out->enabled = false;
        out->detail = utf8_literal("Already in the group");
    }
    return true;
}
// Selecting someone clears the search so the next person can be found.
bool toggle_pick(ApplicationQuickSwitcher& q, u64 user) noexcept {
    u32 index = 0;
    while (index < q.picked_count && q.picked[index] != user) ++index;
    if (index < q.picked_count) {
        q.picked[index] = q.picked[--q.picked_count];
        q.picked[q.picked_count] = 0;
    } else if (q.picked_count < q.picker_limit && q.picked_count < kPrivateChannelRecipientMaximum) {
        q.picked[q.picked_count++] = user;
        if (q.edit.bytes) message_composer_clear(&q.edit);
    } else return true;
    ++q.content_revision;
    return application_quick_switcher_synchronize() && application_request_frame();
}
bool confirm_picks(ApplicationQuickSwitcher& q) noexcept {
    if (!q.picker || !q.picked_count) return true;
    u64 people[kPrivateChannelRecipientMaximum]{};
    const u32 count = q.picked_count;
    const u64 channel = q.picker_channel;
    copy_bytes(people, q.picked, count * sizeof(u64));
    application_quick_switcher_close();
    return channel ? application_private_channel_add(channel, {people, count})
                   : application_private_channel_create({people, count});
}
void revoke_capture(ApplicationQuickSwitcher& q) noexcept {
    q.pressed = {}; q.captured = q.dragging = false;
    ui_widget_manager_capture(&q.scene.widgets, ui_widget_invalid_handle());
    if (GetCapture() == g_app.window) ReleaseCapture();
}
void caret_reset(ApplicationQuickSwitcher& q) noexcept {
    const u32 interval = GetCaretBlinkTime();
    q.caret_visible = true;
    q.blink_at = interval && interval != INFINITE ? deadline_after(clock_monotonic_milliseconds(), interval) : 0;
}
TextInputTarget delegate(const TextInputTarget& target) noexcept {
    return message_composer_input_target(static_cast<MessageComposer*>(target.context));
}
bool edit_ready(const TextInputTarget& target) noexcept {
    const auto* q = g_app.quick_switcher;
    return q && q->active && q->focused && available() && same_scope(q->controller.scope, scope()) &&
        target.context == &q->edit && target.identity == q->identity && text_input_ready(delegate(target));
}
u32 edit_revision(const TextInputTarget& t) noexcept { return text_input_revision(delegate(t)); }
TextInputResult edit_length(const TextInputTarget& t, u32* n) noexcept { return text_input_length(delegate(t), n); }
TextInputResult edit_selection(const TextInputTarget& t, TextInputSelection* s) noexcept { return text_input_selection(delegate(t), s); }
TextInputResult edit_select(const TextInputTarget& t, TextInputSelection s) noexcept { return text_input_select(delegate(t), s); }
TextInputResult edit_copy(const TextInputTarget& t, u32 a, u32 b, Slice<u16> out, u32* n, u32* next) noexcept {
    return text_input_copy(delegate(t), a, b, out, n, next);
}
TextInputResult edit_replace(const TextInputTarget& t, u32 a, u32 b, ConstSlice<u16> in, TextInputChange* c) noexcept {
    if (!c || (!in.data && in.count)) return TextInputResult::invalid_input;
    for (u32 i = 0; i < in.count; ++i)
        if (in[i] < 32 || (in[i] >= 127 && in[i] <= 159) || in[i] == 0x2028 || in[i] == 0x2029)
            return TextInputResult::invalid_input;
    return text_input_replace(delegate(t), a, b, in, c);
}
void edit_cancel(const TextInputTarget& t) noexcept { text_input_cancel(delegate(t)); }
bool edit_begin(const TextInputTarget& t) noexcept {
    auto* m = static_cast<MessageComposer*>(t.context);
    return (m->history || message_composer_enable_history(m, 2048)) && text_input_begin_group(delegate(t));
}
void edit_end(const TextInputTarget& t) noexcept { text_input_end_group(delegate(t)); }
constexpr TextInputOperations edit_operations{edit_ready, edit_revision, edit_length, edit_selection,
    edit_select, edit_copy, edit_replace, edit_cancel, edit_begin, edit_end};
bool pointer_edit(ApplicationQuickSwitcher& q, float x, float y, bool extend) noexcept {
    const auto& layout = g_app.renderer.media_picker_layout;
    const auto text = message_composer_text(q.edit);
    u32 position = 0, byte = 0;
    if (!layout.active || layout.text_signature != text_edit_signature(text) ||
        !text_edit_layout_hit_test(layout, x, y, &position) ||
        !utf8_byte_offset_for_utf16_position(text, position, &byte)) return false;
    message_composer_set_selection(&q.edit, extend ? q.edit.anchor : byte, byte);
    return application_quick_switcher_edit_changed(false);
}
bool activate(ApplicationQuickSwitcher& q, const UiQuickSwitcherTarget& selected) noexcept {
    QuickSwitcherTarget target{};
    if (selected.action == UiQuickSwitcherAction::confirm) return confirm_picks(q);
    if (selected.action != UiQuickSwitcherAction::activate || !ui_quick_switcher_scene_current(q.scene) ||
        !quick_switcher_resolve(q.controller, scope(), sources(), selected.index, selected.source_revision, &target) ||
        selected.key != target.id || selected.kind != ui_kind(target.kind)) return true;
    if (q.picker) return target.kind != QuickSwitcherKind::user || toggle_pick(q, target.user_id);
    // Capture resolved values before close invalidates all borrowed descriptors.
    application_quick_switcher_close();
    switch (target.kind) {
    case QuickSwitcherKind::text_channel:
    case QuickSwitcherKind::private_channel:
    case QuickSwitcherKind::group_private: return application_navigate_notification(target.channel_id, 0);
    case QuickSwitcherKind::guild: return select_content_guild(target.guild_id);
    case QuickSwitcherKind::voice_channel: return application_voice_join(target.guild_id, target.channel_id);
    case QuickSwitcherKind::user:
        if (!queue_private_conversation(PrivateConversationOperation::open, target.user_id)) return false;
        application_friends_hide();
        return synchronize_shell_layout() && refresh_account_text(true) && application_request_frame();
    }
    return true;
}
bool key_tail(UiInputKind kind, u32 key, bool repeated) noexcept {
    if (kind == UiInputKind::character && g_app.quick_switcher_held_key == 'K' && key == 11) return true;
    return ui_input_consume_key_tail(&g_app.quick_switcher_held_key, {kind, key, repeated});
}
} // namespace
bool application_quick_switcher_active() noexcept {
    const auto* q = g_app.quick_switcher;
    return q && q->active && available() && same_scope(q->controller.scope, scope());
}
MessageComposer* application_quick_switcher_edit_model() noexcept {
    return application_quick_switcher_active() ? &g_app.quick_switcher->edit : nullptr;
}
bool application_quick_switcher_edit_focused() noexcept {
    return application_quick_switcher_active() && g_app.quick_switcher->focused;
}
TextInputTarget application_quick_switcher_edit_target() noexcept {
    auto* q = g_app.quick_switcher;
    return application_quick_switcher_edit_focused() ? TextInputTarget{&q->edit, &edit_operations, q->identity} : TextInputTarget{};
}
UiRect application_quick_switcher_edit_bounds() noexcept {
    return application_quick_switcher_active() ? g_app.quick_switcher->scene.text : UiRect{};
}
void application_quick_switcher_edit_cancel_native_group(const TextInputTarget& t) noexcept {
    const auto* q = g_app.quick_switcher;
    if (q && t.context == &q->edit && t.operations == &edit_operations && t.identity == q->identity) {
        text_input_end_group(delegate(t)); text_input_cancel(delegate(t));
    }
}
void application_quick_switcher_close() noexcept {
    auto* q = g_app.quick_switcher;
    if (!q || !q->active) return;
    application_composer_text_input_suspend();
    revoke_capture(*q); q->active = q->focused = false; q->blink_at = 0;
    q->edit.focused = false; q->picker = false; q->picked_count = 0; q->picker_channel = 0;
    message_composer_reset(&q->edit, 0, false);
    g_app.renderer.quick_switcher_scene = nullptr;
    if (g_app.context_focus == &q->scene.widgets) g_app.context_focus = nullptr;
    renderer_bind_focus_owner(&g_app.renderer, nullptr);
    q->scene.active = false;
    q->scene.source = nullptr;
    q->scene.content = nullptr;
    application_surface_motion_retire(RendererLayerSurface::popup);
    application_request_frame();
}
void application_quick_switcher_release() noexcept {
    application_quick_switcher_close();
    auto* q = g_app.quick_switcher;
    if (q) {
        message_composer_release(&q->edit);
        ui_quick_switcher_scene_release(&q->scene);
        g_app.quick_switcher = nullptr;
        SecureZeroMemory(q, sizeof(*q)); VirtualFree(q, 0, MEM_RELEASE);
    }
    g_app.quick_switcher_recent_scope = {};
    g_app.quick_switcher_recent_count = 0;
    SecureZeroMemory(g_app.quick_switcher_recent_channels, sizeof(g_app.quick_switcher_recent_channels));
}
void application_quick_switcher_note_channel(u64 channel) noexcept {
    recent_scope();
    const auto bank_scope = scope();
    if (!channel || !g_app.account_ui || !g_app.account.instance || g_app.account.stop_failed ||
        !bank_scope.account_serial || !bank_scope.profile_key || !bank_scope.self_user_id) return;
    const auto* record = chat_state_store_find_channel(g_app.account_ui->chat_state, channel);
    if (!record) return;
    u32 found = 0;
    while (found < g_app.quick_switcher_recent_count && g_app.quick_switcher_recent_channels[found] != channel) ++found;
    if (found == g_app.quick_switcher_recent_count && g_app.quick_switcher_recent_count < kQuickSwitcherRecentMaximum)
        ++g_app.quick_switcher_recent_count;
    for (u32 i = minimum(found, kQuickSwitcherRecentMaximum - 1); i > 0; --i)
        g_app.quick_switcher_recent_channels[i] = g_app.quick_switcher_recent_channels[i - 1];
    g_app.quick_switcher_recent_channels[0] = channel;
    if (g_app.quick_switcher)
        quick_switcher_visit(&g_app.quick_switcher->controller, scope(), sources(), channel);
}
bool application_quick_switcher_synchronize() noexcept {
    recent_scope();
    auto* q = g_app.quick_switcher;
    if (!q || !q->active) return true;
    if (!available() || !same_scope(q->controller.scope, scope())) { application_quick_switcher_close(); return true; }
    const auto previous = q->controller.revision;
    if (q->picker && q->picker_channel &&
        !private_channel_member_count(g_app.account_ui->chat_state, q->picker_channel)) {
        application_quick_switcher_close();
        return true;
    }
    quick_switcher_synchronize(&q->controller, scope(), sources(), message_composer_text(q->edit),
                               q->picker ? QuickSwitcherMode::friends : QuickSwitcherMode::navigation);
    if (q->controller.revision != previous) revoke_capture(*q);
    q->source = {q, q->controller.count, q->controller.revision, resolve_row, source_current};
    const auto phase = !q->controller.count ? UiQuickSwitcherPhase::no_matches : UiQuickSwitcherPhase::ready;
    q->content = {{message_composer_text(q->edit), q->edit.anchor, q->edit.caret, true, q->focused, q->caret_visible},
        utf8_literal("Quick switcher"), utf8_literal("@ people   # channels   ! voice   * servers"),
        q->controller.phase == QuickSwitcherPhase::invalid_query ? utf8_literal("Enter a shorter search.")
        : q->controller.count ? Utf8View{} : utf8_literal("No matches"), q->content_revision, phase};
    if (q->picker) {
        const u32 left = q->picker_limit > q->picked_count ? q->picker_limit - q->picked_count : 0;
        ProtocolWriter hint{{reinterpret_cast<u8*>(q->hint), sizeof(q->hint)}, 0};
        const bool written = left ? protocol_writer_append_c_string(&hint, "You can add ") &&
                                        protocol_writer_append_u64(&hint, left) &&
                                        protocol_writer_append_c_string(&hint, left == 1 ? " more friend." : " more friends.")
                                  : protocol_writer_append_c_string(&hint, "Groups are limited to 10 members.");
        ProtocolWriter status{{reinterpret_cast<u8*>(q->status), sizeof(q->status)}, 0};
        const bool counted = !q->picked_count || (protocol_writer_append_u64(&status, q->picked_count) &&
                                                  protocol_writer_append_c_string(&status, " selected"));
        const auto* channel = q->picker_channel
            ? chat_state_store_find_channel(g_app.account_ui->chat_state, q->picker_channel) : nullptr;
        q->content.heading = utf8_literal("Select friends");
        q->content.hint = written ? Utf8View{q->hint, hint.used} : Utf8View{};
        q->content.status = counted ? Utf8View{q->status, status.used} : Utf8View{};
        q->content.picker = true;
        q->content.can_confirm = q->picked_count != 0;
        q->content.action = channel && channel->type == 3 ? utf8_literal("Add")
                            : q->picked_count > 1 || channel ? utf8_literal("Create group DM")
                                                             : utf8_literal("Create DM");
        q->content.checked = {q->picked, q->picked_count};
    }
    if (!ui_quick_switcher_scene_update(&q->scene, &q->source, &q->content, application_viewport_dips())) return false;
    g_app.renderer.quick_switcher_scene = &q->scene;
    g_app.context_focus = &q->scene.widgets;
    ui_widget_manager_focus(&q->scene.widgets, q->scene.edit);
    return true;
}
bool application_quick_switcher_open() noexcept {
    if (!available()) return true;
    if (application_quick_switcher_active()) return true;
    application_composer_text_input_suspend();
    application_media_picker_close(); application_pinned_messages_close(); application_search_close_panel();
    application_account_menu_clear();
    if (!application_profile_close() || !dismiss_popup()) return false;
    recent_scope();
    auto* q = g_app.quick_switcher;
    if (!q) {
        q = static_cast<ApplicationQuickSwitcher*>(VirtualAlloc(nullptr, sizeof(*q), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!q) return false;
        if (!message_composer_initialize(&q->edit, {q->query, sizeof(q->query)})) { VirtualFree(q, 0, MEM_RELEASE); return false; }
        g_app.quick_switcher = q;
    }
    if (g_app.quick_switcher_lifetime_serial == ~u64{0}) return false;
    q->identity = ++g_app.quick_switcher_lifetime_serial;
    q->active = q->focused = q->edit.focused = true;
    q->controller = {};
    for (u32 i = g_app.quick_switcher_recent_count; i > 0; --i)
        quick_switcher_visit(&q->controller, scope(), sources(), g_app.quick_switcher_recent_channels[i - 1]);
    // An empty MRU still needs an initialized scope before publication.
    quick_switcher_synchronize(&q->controller, scope(), sources(), message_composer_text(q->edit));
    ++q->content_revision; caret_reset(*q);
    if (!application_quick_switcher_synchronize()) { application_quick_switcher_close(); return false; }
    application_video_blur();
    if (g_app.window) SetFocus(g_app.window);
    return application_composer_text_input_synchronize() && application_request_frame();
}
bool application_quick_switcher_open_picker(u64 channel_id) noexcept {
    if (!available() || !g_app.account_ui) return true;
    const u32 limit = channel_id ? private_channel_capacity_left(g_app.account_ui->chat_state, channel_id)
                                 : kPrivateChannelRecipientMaximum;
    if (!limit) return true;
    if (application_quick_switcher_active()) application_quick_switcher_close();
    if (!application_quick_switcher_open() || !g_app.quick_switcher || !g_app.quick_switcher->active) return false;
    auto* q = g_app.quick_switcher;
    q->picker = true;
    q->picker_channel = channel_id;
    q->picker_limit = minimum(limit, kPrivateChannelRecipientMaximum);
    q->picked_count = 0;
    ++q->content_revision;
    if (!application_quick_switcher_synchronize()) { application_quick_switcher_close(); return false; }
    return application_request_frame();
}
bool application_quick_switcher_toggle() noexcept {
    if (application_quick_switcher_active()) { application_quick_switcher_close(); return true; }
    return application_quick_switcher_open();
}
bool application_quick_switcher_edit_changed(bool) noexcept {
    auto* q = g_app.quick_switcher;
    if (!application_quick_switcher_active()) return true;
    ++q->content_revision; caret_reset(*q);
    return application_quick_switcher_synchronize() && application_request_frame();
}
bool application_quick_switcher_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    if (key_tail(kind, key, repeated)) return true;
    const bool ctrl_raw = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool control = ctrl_raw && !alt, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (kind == UiInputKind::key_down && shortcut_matches(ShortcutAction::quick_switcher, shortcut_chord_from_keyboard(key)) &&
        available()) {
        if (!repeated) { g_app.quick_switcher_held_key = key; application_quick_switcher_toggle(); }
        return true;
    }
    auto* q = g_app.quick_switcher;
    if (!q || !q->active) return false;
    if (!application_quick_switcher_active()) { application_quick_switcher_close(); return true; }
    if (kind == UiInputKind::key_up) return true;
    if (kind == UiInputKind::character) {
        if (control || (alt && !ctrl_raw) || key < 32 || (key >= 127 && key <= 159) || key == 0x2028 || key == 0x2029 || key > 0xffff) return true;
        if (!q->edit.history) message_composer_enable_history(&q->edit, 2048);
        if (message_composer_insert_utf16(&q->edit, static_cast<u16>(key), clock_monotonic_milliseconds()) == MessageComposerResult::changed)
            application_quick_switcher_edit_changed(true);
        return true;
    }
    if (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down) return true;
    if (key == VK_ESCAPE) { g_app.quick_switcher_held_key = key; application_quick_switcher_close(); return true; }
    if (key == VK_RETURN) {
        g_app.quick_switcher_held_key = key;
        if (!repeated && !application_composer_text_input_composing())
            activate(*q, q->picker && control ? UiQuickSwitcherTarget{UiQuickSwitcherAction::confirm}
                                              : ui_quick_switcher_scene_selected(q->scene));
        return true;
    }
    if (key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT) {
        ui_quick_switcher_scene_move(&q->scene, key == VK_UP ? UiQuickSwitcherMove::previous :
            key == VK_DOWN ? UiQuickSwitcherMove::next : key == VK_PRIOR ? UiQuickSwitcherMove::page_previous : UiQuickSwitcherMove::page_next);
        application_request_frame(); return true;
    }
    const auto revision = q->edit.text_revision;
    MessageComposerResult result = MessageComposerResult::unchanged;
    if (control && (key == 'C' || key == 'X')) {
        const u32 a = minimum(q->edit.anchor, q->edit.caret), b = maximum(q->edit.anchor, q->edit.caret);
        if (b > a && clipboard_write_utf8(g_app.window, {q->edit.text + a, b - a}) && key == 'X')
            result = message_composer_replace_range(&q->edit, a, b, {});
    } else if (control && key == 'V') {
        char text[kQuickSwitcherQueryMaximumBytes];
        const auto read = clipboard_read_utf8(g_app.window, {text, sizeof(text)});
        bool valid = read.ok();
        for (u32 i = 0; valid && i < read.bytes;) {
            const auto scalar = utf8_decode_one(text + i, read.bytes - i);
            valid = scalar.valid && scalar.codepoint >= 32 && !(scalar.codepoint >= 127 && scalar.codepoint <= 159) && scalar.codepoint != 0x2028 && scalar.codepoint != 0x2029;
            if (valid) i += scalar.width;
        }
        if (valid) result = message_composer_insert_utf8(&q->edit, {text, read.bytes});
    } else {
        if (key == VK_BACK) g_app.quick_switcher_held_key = key;
        message_composer_edit_key(&q->edit, key, control, shift, &result);
    }
    if (result == MessageComposerResult::changed) application_quick_switcher_edit_changed(revision != q->edit.text_revision);
    return true;
}
bool application_quick_switcher_filter(const MSG& message) noexcept {
    if (message.hwnd != g_app.window || (message.message != WM_KEYDOWN && message.message != WM_KEYUP)) return false;
    const auto kind = message.message == WM_KEYDOWN ? UiInputKind::key_down : UiInputKind::key_up;
    const bool repeated = (message.lParam & (1ll << 30)) != 0;
    if (key_tail(kind, static_cast<u32>(message.wParam), repeated)) return true;
    const u32 key = static_cast<u32>(message.wParam);
    if (message.message == WM_KEYDOWN &&
        shortcut_matches(ShortcutAction::quick_switcher, shortcut_chord_from_keyboard(key)) && available())
        return application_quick_switcher_input(kind, key, repeated);
    return false;
}
bool application_quick_switcher_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* q = g_app.quick_switcher;
    if (!q || !q->active) return false;
    if (!application_quick_switcher_active()) { application_quick_switcher_close(); return false; }
    if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) {
        UiPoint point{};
        if (!application_cursor_point(g_app.window, g_app.renderer.dpi, lparam, &point)) return false;
        application_set_cursor(ui_quick_switcher_scene_cursor(q->scene, point.x, point.y));
        return true;
    }
    if (message == WM_KILLFOCUS || message == WM_CAPTURECHANGED || message == WM_CANCELMODE) {
        revoke_capture(*q); q->blink_at = 0; q->caret_visible = false; ++q->content_revision;
        application_request_frame(); return false;
    }
    if (message == WM_SETFOCUS) { caret_reset(*q); ++q->content_revision; application_request_frame(); return false; }
    if (message == WM_CONTEXTMENU || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
        message == WM_MBUTTONDOWN || message == WM_MBUTTONUP || message == WM_LBUTTONDBLCLK || message == WM_MOUSEHWHEEL) return true;
    if (message != WM_LBUTTONDOWN && message != WM_LBUTTONUP && message != WM_MOUSEMOVE && message != WM_MOUSEWHEEL) return false;
    POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    if (message == WM_MOUSEWHEEL) ScreenToClient(g_app.window, &point);
    const float scale = 96.f / maximum(1u, g_app.renderer.dpi), x = point.x * scale, y = point.y * scale;
    if (message == WM_MOUSEWHEEL) {
        ui_quick_switcher_scene_scroll(&q->scene, q->scene.scroll_dips -
            static_cast<short>(HIWORD(wparam)) / static_cast<float>(WHEEL_DELTA) * kUiQuickSwitcherRowHeight * 3);
        application_request_frame(); return true;
    }
    if (message == WM_MOUSEMOVE) {
        if (q->dragging && (wparam & MK_LBUTTON)) pointer_edit(*q, x, y, true);
        else { bool changed = false; ui_quick_switcher_scene_set_hover(&q->scene, x, y, &changed); if (changed) application_request_frame(); }
        return true;
    }
    const auto hit = ui_quick_switcher_scene_hit_test(q->scene, x, y);
    if (message == WM_LBUTTONDOWN) {
        q->pressed = hit; q->captured = true; q->dragging = hit.action == UiQuickSwitcherAction::edit;
        if (q->dragging) pointer_edit(*q, x, y, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        if (g_app.window) SetCapture(g_app.window);
        return true;
    }
    const auto pressed = q->pressed; const bool captured = q->captured;
    revoke_capture(*q);
    if (captured && pressed.action == hit.action && pressed.key == hit.key && pressed.source_revision == hit.source_revision) {
        if (hit.action == UiQuickSwitcherAction::close) application_quick_switcher_close();
        else if (hit.action == UiQuickSwitcherAction::activate || hit.action == UiQuickSwitcherAction::confirm)
            activate(*q, hit);
    }
    return true;
}
u64 application_quick_switcher_caret_wake() noexcept {
    return application_quick_switcher_edit_focused() && GetFocus() == g_app.window ? g_app.quick_switcher->blink_at : 0;
}
void application_quick_switcher_tick(u64 now) noexcept {
    auto* q = g_app.quick_switcher;
    if (!q || !deadline_due(now, application_quick_switcher_caret_wake())) return;
    q->caret_visible = !q->caret_visible; ++q->content_revision;
    const u32 interval = GetCaretBlinkTime();
    q->blink_at = interval && interval != INFINITE ? deadline_after(now, interval) : 0;
    application_request_frame();
}
u32 application_quick_switcher_owner_bytes() noexcept { return g_app.quick_switcher ? sizeof(ApplicationQuickSwitcher) : 0; }
} // namespace maicord::app_detail
