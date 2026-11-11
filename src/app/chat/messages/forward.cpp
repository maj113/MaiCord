#include "forward.hpp"
#include "maicord/model/chat/forwarded_input.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "../../shell/overlay_input.hpp"
#include "../../shell/input.hpp"
#include "../../shell/window_events.hpp"
#include "../../shell/window_state.hpp"
#include "../composer/text_edit_keys.hpp"
#include "../../shell/cursor.hpp"
#include "maicord/ui/core/scene_cursor.hpp"
#include "../../shell/state.hpp"
#include "../../account/account_presentation.hpp"
#include "../composer/text_input.hpp"
#include "../../account/friends.hpp"
#include "../../media/picker.hpp"
#include "../search/search.hpp"
#include "pinned_messages.hpp"
#include "../../account/profile.hpp"
#include "interaction.hpp"
#include "../../account/account_menu.hpp"
#include "../../shell/surface_motion.hpp"
#include "../../settings/theme_editor.hpp"
#include "../../shell/quick_switcher.hpp"
#include "../../media/video.hpp"
#include "maicord/app/chat/messages/forward_controller.hpp"
#include "maicord/protocol/messages/card_excerpt.hpp"
#include "maicord/ui/chat/messages/forward_scene.hpp"
#include "maicord/platform/window/clipboard.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/render/text/form_surface.hpp"
#include "../../account/account_ui_runtime.hpp"

#include "maicord/platform/window/key.hpp"

namespace maicord::app_detail {
struct ApplicationMessageForward {
    MessageForwardController controller;
    AccountUiRuntime* account_ui;
    void* account_instance;
    MessageComposer edit, caption;
    UiMessageForwardScene scene;
    UiMessageForwardSource source;
    UiMessageForwardContent content;
    UiMessageForwardTarget pressed;
    char query[kQuickSwitcherQueryMaximumBytes], caption_text[kMessageForwardCaptionMaximumBytes];
    u64 identity, blink_at;
    u32 content_revision;
    bool active, focused, caption_focused, caret_visible, captured, dragging, resumed_existing;
};
namespace {
QuickSwitcherScope scope() noexcept {
    return {g_app.plugin_account_scope_serial, g_app.account.status.profile_id,
            g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0};
}
QuickSwitcherSources sources() noexcept {
    return g_app.account_ui ? QuickSwitcherSources{&g_app.account_ui->chat_state,
        &g_app.account_ui->user_store, application_friends_relationships()} : QuickSwitcherSources{};
}
bool available() noexcept {
    const auto s = scope();
    return g_app.renderer_ready && g_app.account_ui && g_app.account.instance && !g_app.account.stop_failed &&
        s.account_serial && s.profile_key && s.self_user_id && !g_app.login.visible &&
        !(g_app.renderer.text_form && g_app.renderer.text_form->scene && g_app.renderer.text_form->scene->active) &&
        !application_theme_editor_active() &&
        !(g_app.renderer.settings_scene && g_app.renderer.settings_scene->active) &&
        !g_app.image_viewer.scene.active && !g_app.reaction_viewer.active;
}
UiQuickSwitcherKind ui_kind(QuickSwitcherKind kind) noexcept {
    return kind == QuickSwitcherKind::private_channel ? UiQuickSwitcherKind::private_channel :
        kind == QuickSwitcherKind::group_private ? UiQuickSwitcherKind::group_private : UiQuickSwitcherKind::text_channel;
}
bool source_current(void* opaque) noexcept {
    const auto* q = static_cast<const ApplicationMessageForward*>(opaque);
    return q && q == g_app.message_forward && q->active && available() &&
        message_forward_current(q->controller, scope(), sources()) &&
        quick_switcher_current(q->controller.candidates, scope(), sources(), q->source.revision);
}
bool resolve_row(void* opaque, u32 index, UiMessageForwardRow* out) noexcept {
    auto* q = static_cast<ApplicationMessageForward*>(opaque);
    QuickSwitcherTarget target{};
    if (!source_current(q) || !quick_switcher_resolve(q->controller.candidates,
        scope(), sources(), index, q->source.revision, &target)) return false;
    const auto& row = q->controller.candidates.rows[index];
    *out = {target.id, target.guild_id, target.channel_id, target.user_id, ui_kind(target.kind),
        {row.label, row.label_bytes}, {row.detail, row.detail_bytes}, {}, {row.avatar_hash, row.avatar_hash_bytes}, true};
    return true;
}
void revoke_capture(ApplicationMessageForward& q) noexcept {
    g_app.message_forward_pointer_tail = g_app.message_forward_pointer_tail || q.captured;
    reset_value(&q.pressed); q.captured = q.dragging = false;
    ui_widget_manager_capture(&q.scene.widgets, ui_widget_invalid_handle());
    application_release_capture();
}
u64 blink_deadline(u64 now) noexcept {
    const u32 interval = application_caret_blink_ms();
    return interval && interval != INFINITE ? deadline_after(now, interval) : 0;
}
void caret_reset(ApplicationMessageForward& q) noexcept {
    q.caret_visible = true;
    q.blink_at = blink_deadline(clock_monotonic_milliseconds());
}
bool edit_ready(const TextInputTarget& target) noexcept {
    const auto* q = g_app.message_forward;
    return application_message_forward_edit_focused() &&
        target.context == (q->caption_focused ? &q->caption : &q->edit) && target.identity == q->identity;
}
using EditInput = ForwardedInput<edit_ready, text_input_single_line_forbidden>;
bool pointer_edit(ApplicationMessageForward& q, float x, float y, bool extend) noexcept {
    const auto& layout = q.caption_focused ? g_app.renderer.popup_edit_layout : g_app.renderer.media_picker_layout;
    auto& model = q.caption_focused ? q.caption : q.edit;
    const auto text = message_composer_text(model);
    u32 position = 0, byte = 0;
    if (!layout.active || layout.text_signature != text_edit_signature(text) ||
        !text_edit_layout_hit_test(layout, x, y, &position) ||
        !utf8_byte_offset_for_utf16_position(text, position, &byte)) return false;
    message_composer_set_selection(&model, extend ? model.anchor : byte, byte);
    return application_message_forward_edit_changed(false);
}

bool focus_field(ApplicationMessageForward& q, bool caption) noexcept {
    if (q.controller.committed) return true;
    application_composer_text_input_suspend();
    q.caption_focused = caption; q.focused = true;
    q.edit.focused = !caption; q.caption.focused = caption;
    caret_reset(q); ++q.content_revision;
    return application_message_forward_synchronize() && application_composer_text_input_synchronize() && application_request_frame();
}
bool activate(ApplicationMessageForward& q, UiMessageForwardTarget target) noexcept {
    if (target.action == UiMessageForwardAction::close) { application_message_forward_close(); return true; }
    if (!ui_message_forward_scene_current(q.scene)) return true;
    switch (target.action) {
    case UiMessageForwardAction::cancel_pending:
        if (g_app.account_ui)
            message_forward_cancel_pending(&q.controller, &g_app.account_ui->message_composer_controller);
        if (!message_forward_pending(q.controller)) { application_message_forward_close(); return true; }
        break;
    case UiMessageForwardAction::edit: return focus_field(q, false);
    case UiMessageForwardAction::caption: return focus_field(q, true);
    case UiMessageForwardAction::toggle:
        message_forward_toggle(&q.controller, scope(), sources(), target.index, target.source_revision); break;
    case UiMessageForwardAction::remove: message_forward_remove(&q.controller, target.channel_id); break;
    case UiMessageForwardAction::send:
    case UiMessageForwardAction::retry:
        if (application_composer_text_input_composing()) return true;
        if (!g_app.account_ui || g_app.account.status.required_action != AccountRequirement::none) return true;
        application_composer_text_input_suspend();
        message_forward_set_caption(&q.controller, message_composer_text(q.caption));
        if (target.action == UiMessageForwardAction::retry)
            message_forward_retry_failed(&q.controller, scope(), sources(), &g_app.account_ui->message_composer_controller,
                clock_unix_milliseconds(), clock_monotonic_milliseconds(), target.channel_id);
        else message_forward_confirm(&q.controller, scope(), sources(), &g_app.account_ui->message_composer_controller,
                clock_unix_milliseconds(), clock_monotonic_milliseconds());
        if (q.controller.committed) q.focused = q.edit.focused = q.caption.focused = false;
        if (g_app.window) window_wake(g_app.window);
        break;
    default: break;
    }
    ++q.content_revision;
    return application_message_forward_synchronize() && application_request_frame();
}
} // namespace
bool application_message_forward_active() noexcept {
    const auto* q = g_app.message_forward;
    return q && q->active;
}
bool application_message_forward_available(u64 id) noexcept {
    if (!available() || !id || g_app.account.status.required_action != AccountRequirement::none) return false;
    const auto found = message_store_find(g_app.message_view.store, id);
    const auto* record = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (!record || record->delivery != MessageDeliveryState::authoritative ||
        record->channel_id != g_app.account_ui->active_channel_id) return false;
    return message_forward_source_available(sources(), g_app.message_view.store, found.handle);
}
MessageComposer* application_message_forward_edit_model() noexcept {
    auto* q = g_app.message_forward;
    return application_message_forward_edit_focused() ? (q->caption_focused ? &q->caption : &q->edit) : nullptr;
}
bool application_message_forward_edit_focused() noexcept {
    const auto* q = g_app.message_forward;
    return q && q->active && q->focused && !q->controller.committed && available() && quick_switcher_scope_equal(q->controller.scope, scope());
}
bool application_message_forward_caption_focused() noexcept {
    return application_message_forward_edit_focused() && g_app.message_forward->caption_focused;
}
TextInputTarget application_message_forward_edit_target() noexcept {
    auto* model = application_message_forward_edit_model();
    return model ? TextInputTarget{model, &EditInput::operations, g_app.message_forward->identity} : TextInputTarget{};
}
UiRect application_message_forward_edit_bounds() noexcept {
    auto* q = g_app.message_forward;
    return q && q->active ? (q->caption_focused ? q->scene.caption_text : q->scene.text) : UiRect{};
}
void application_message_forward_edit_cancel_native_group(const TextInputTarget& t) noexcept {
    const auto* q = g_app.message_forward;
    if (q && (t.context == &q->edit || t.context == &q->caption) && t.operations == &EditInput::operations && t.identity == q->identity) {
        EditInput::abandon(t);
    }
}
void application_message_forward_close() noexcept {
    auto* q = g_app.message_forward;
    if (!q || !q->active) return;
    application_composer_text_input_suspend();
    revoke_capture(*q); q->active = q->focused = false; q->blink_at = 0;
    q->edit.focused = q->caption.focused = false;
    g_app.renderer.message_forward_scene = nullptr;
    if (g_app.context_focus == &q->scene.widgets) g_app.context_focus = nullptr;
    renderer_bind_focus_owner(&g_app.renderer, nullptr);
    q->scene.active = false; q->scene.source = nullptr; q->scene.content = nullptr;
    application_surface_motion_retire(RendererLayerSurface::popup);
    application_request_frame();
}
void application_message_forward_release() noexcept {
    application_message_forward_close();
    auto* q = g_app.message_forward;
    if (!q) return;
    message_composer_release(&q->edit); message_composer_release(&q->caption);
    ui_message_forward_scene_release(&q->scene);
    g_app.message_forward = nullptr;
    SecureZeroMemory(q, sizeof(*q)); pages_release(q);
}
bool application_message_forward_synchronize() noexcept {
    auto* q = g_app.message_forward;
    if (!q) return true;
    if (!g_app.account_ui) { application_message_forward_close(); return true; }
    const QuickSwitcherScope current = scope();
    if (!quick_switcher_scope_equal(q->controller.scope, current)) {
        // Never apply an old account's nonces to another account's outbox.
        if (q->account_ui == g_app.account_ui && q->account_instance == g_app.account.instance &&
            q->controller.scope.profile_key == current.profile_key && q->controller.scope.self_user_id == current.self_user_id)
            message_forward_cancel_pending(&q->controller, &g_app.account_ui->message_composer_controller);
        application_message_forward_close();
        reset_value(&q->controller);
        message_composer_reset(&q->edit, 0, false); message_composer_reset(&q->caption, 0, false);
        return true;
    }
    const u32 previous = q->controller.candidates.revision;
    message_forward_synchronize(&q->controller, current, sources(), message_composer_text(q->edit),
        &g_app.account_ui->message_composer_controller);
    if (!q->controller.initialized || !available()) { application_message_forward_close(); return true; }
    if (!q->active) return true;
    if (q->controller.candidates.revision != previous) revoke_capture(*q);
    q->source = {q, q->controller.candidates.count, q->controller.candidates.revision, resolve_row, source_current};
    reset_value(&q->content);
    q->content.edit = {message_composer_text(q->edit), q->edit.anchor, q->edit.caret, !q->controller.committed,
        q->focused && !q->caption_focused, q->caret_visible};
    q->content.caption = {message_composer_text(q->caption), q->caption.anchor, q->caption.caret, !q->controller.committed,
        q->focused && q->caption_focused, q->caret_visible};
    q->content.author = {q->controller.source.author, q->controller.source.author_bytes};
    q->content.preview = {q->controller.source.preview, q->controller.source.preview_bytes};
    q->content.media_summary = {q->controller.source.media_summary, q->controller.source.media_summary_bytes};
    q->content.status = q->resumed_existing ? utf8_literal("Finish or cancel the previous forward before starting another.") : message_forward_status(q->controller);
    q->content.revision = q->content_revision + q->controller.revision;
    q->content.selected_count = q->controller.destination_count;
    q->content.phase = q->source.count ? UiMessageForwardPhase::ready : UiMessageForwardPhase::no_matches;
    q->content.can_send = !q->controller.committed && q->controller.caption_valid && q->controller.destination_count;
    q->content.sending = message_forward_pending(q->controller);
    q->content.committed = q->controller.committed;
    // UI state by MessageForwardDestinationState.
    static constexpr UiMessageForwardState kStates[] = {
        UiMessageForwardState::ready, UiMessageForwardState::sending, UiMessageForwardState::sending,
        UiMessageForwardState::sending, UiMessageForwardState::sent, UiMessageForwardState::failed,
        UiMessageForwardState::failed, UiMessageForwardState::cancelled};
    for (u32 i = 0; i < q->controller.destination_count; ++i) {
        const auto& d = q->controller.destinations[i];
        if (!q->controller.committed && d.state != MessageForwardDestinationState::ready) q->content.can_send = false;
        q->content.selected[i] = {d.row.target.channel_id, {d.row.label, d.row.label_bytes}, message_forward_destination_status(d),
            kStates[static_cast<u32>(d.state)]};
    }
    if (!ui_message_forward_scene_update(&q->scene, &q->source, &q->content, application_viewport_dips())) return false;
    g_app.renderer.message_forward_scene = &q->scene;
    g_app.context_focus = &q->scene.widgets;
    ui_widget_manager_focus(&q->scene.widgets, q->focused ? (q->caption_focused ? q->scene.caption : q->scene.edit) : q->scene.send);
    return true;
}
bool application_message_forward_open(u64 id) noexcept {
    if (!application_message_forward_available(id)) return true;
    auto* q = g_app.message_forward;
    bool resume = false;
    if (q && q->controller.initialized && q->controller.committed)
        for (u32 i = 0; i < q->controller.destination_count; ++i)
            if (q->controller.destinations[i].state != MessageForwardDestinationState::sent &&
                q->controller.destinations[i].state != MessageForwardDestinationState::cancelled) resume = true;
    if (resume) {
        q->resumed_existing = q->controller.source.reference.message_id != id;
        application_composer_text_input_suspend(); application_quick_switcher_close(); application_video_blur();
        application_media_picker_close(); application_pinned_messages_close(); application_search_close_panel();
        application_account_menu_clear();
        if (!application_profile_close() || !dismiss_popup()) return false;
        application_release_capture();
        q->active = true;
        if (g_app.window) window_focus(g_app.window);
        return application_message_forward_synchronize() && application_request_frame();
    }
    // Retire a completed publication before replacing its source/candidate bank.
    application_message_forward_close();
    const auto found = message_store_find(g_app.message_view.store, id);
    const auto* record = message_store_resolve(g_app.message_view.store, found.handle);
    if (!record) return true;
    application_composer_text_input_suspend();
    application_quick_switcher_close(); application_media_picker_close(); application_pinned_messages_close(); application_search_close_panel();
    application_account_menu_clear();
    if (!application_profile_close() || !dismiss_popup()) return false;
    if (!q) {
        q = static_cast<ApplicationMessageForward*>(commit_pages(sizeof(*q)));
        if (!q) return false;
        if (!message_composer_initialize(&q->edit, {q->query, sizeof(q->query)}) ||
            !message_composer_initialize(&q->caption, {q->caption_text, sizeof(q->caption_text)})) {
            message_composer_release(&q->edit); message_composer_release(&q->caption); pages_release(q); return false;
        }
        g_app.message_forward = q;
    }
    q->identity = ++g_app.message_forward_lifetime_serial;
    message_composer_reset(&q->edit, 0, false); message_composer_reset(&q->caption, 0, false);
    const auto* author = user_store_find(g_app.account_ui->user_store, record->author_id);
    const auto* rich = message_rich_store_find(g_app.account_ui->message_rich_store, id);
    Utf8View summary = rich && rich->attachment_count ? utf8_literal("Attachments included") :
        rich && rich->embed_count ? utf8_literal("Embedded content included") :
        rich && rich->has_forwarded_snapshot ? utf8_literal("Forwarded content included") : Utf8View{};
    if (!message_forward_open(&q->controller, scope(), sources(), &g_app.message_view.store, found.handle,
        author ? user_store_name(g_app.account_ui->user_store, *author) : utf8_literal("Message"), summary)) return true;
    if (!record->content_bytes && rich && rich->message_revision == record->revision && rich->has_forwarded_snapshot) {
        const auto* child = message_rich_store_snapshot(g_app.account_ui->message_rich_store, id);
        if (child) {
            const auto excerpt = message_card_excerpt({{},
                message_rich_store_string(g_app.account_ui->message_rich_store, child->snapshot.content), true, true,
                child->attachment_count, child->embed_count, child->sticker_count, child->component_count},
                {q->controller.source.preview, sizeof(q->controller.source.preview)});
            q->controller.source.preview_bytes = excerpt.bytes;
        }
    }
    q->account_ui = g_app.account_ui; q->account_instance = g_app.account.instance;
    q->active = q->focused = q->edit.focused = true; q->caption_focused = q->resumed_existing = false; g_app.message_forward_pointer_tail = false;
    ++q->content_revision; caret_reset(*q);
    application_video_blur();
    application_release_capture();
    if (!application_message_forward_synchronize()) { application_message_forward_close(); return false; }
    if (g_app.window) window_focus(g_app.window);
    return application_composer_text_input_synchronize() && application_request_frame();
}
bool application_message_forward_edit_changed(bool content) noexcept {
    auto* q = g_app.message_forward;
    if (!application_message_forward_edit_focused()) return true;
    if (content && q->caption_focused) message_forward_set_caption(&q->controller, message_composer_text(q->caption));
    ++q->content_revision; caret_reset(*q);
    return application_message_forward_synchronize() && application_request_frame();
}
static_assert(kMessageForwardCaptionMaximumBytes <= kPasteLineMaximumBytes);
bool application_message_forward_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    if (ui_input_consume_key_tail(&g_app.message_forward_held_key, {kind, key, repeated})) return true;
    auto* q = g_app.message_forward;
    if (!q || !q->active) return false;
    if (!available() || !quick_switcher_scope_equal(q->controller.scope, scope())) {
        if (kind == UiInputKind::key_down || kind == UiInputKind::system_key_down)
            g_app.message_forward_held_key = key;
        application_message_forward_close(); return true;
    }
    const bool ctrl_raw = application_control_down(), alt = application_alt_down();
    const bool control = ctrl_raw && !alt, shift = application_shift_down();
    if (kind == UiInputKind::key_up) return true;
    if (kind == UiInputKind::key_down && key == key::escape) { g_app.message_forward_held_key = key; application_message_forward_close(); return true; }
    if (kind == UiInputKind::key_down && key == key::tab) {
        g_app.message_forward_held_key = key; if (!repeated) focus_field(*q, !q->caption_focused); return true;
    }
    if (kind == UiInputKind::key_down && key == key::enter) {
        g_app.message_forward_held_key = key;
        if (!repeated && !application_composer_text_input_composing()) {
            if (control || q->caption_focused) activate(*q, {q->controller.committed ? UiMessageForwardAction::retry : UiMessageForwardAction::send});
            else activate(*q, ui_message_forward_scene_selected(q->scene));
        }
        return true;
    }
    if (kind == UiInputKind::key_down && (key == key::up || key == key::down || key == key::page_up || key == key::page_down)) {
        ui_message_forward_scene_move(&q->scene, key == key::up ? UiQuickSwitcherMove::previous :
            key == key::down ? UiQuickSwitcherMove::next : key == key::page_up ? UiQuickSwitcherMove::page_previous : UiQuickSwitcherMove::page_next);
        application_request_frame(); return true;
    }
    if (!application_message_forward_edit_focused()) return true;
    auto& model = q->caption_focused ? q->caption : q->edit;
    if (kind == UiInputKind::character) {
        if (control || (alt && !ctrl_raw) || text_input_single_line_forbidden(key) || key > 0xffff) return true;
        if (!model.history) message_composer_enable_history(&model, 2048);
        if (message_composer_insert_utf16(&model, static_cast<u16>(key), clock_monotonic_milliseconds()) == MessageComposerResult::changed)
            application_message_forward_edit_changed(true);
        return true;
    }
    if (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down) return true;
    const auto revision = model.text_revision;
    MessageComposerResult result = MessageComposerResult::unchanged;
    if (control && key == 'V') {
        result = message_composer_paste_line(&model, kMessageForwardCaptionMaximumBytes,
                                             text_input_single_line_forbidden, false);
    } else {
        if (key == key::backspace) g_app.message_forward_held_key = key;
        message_composer_edit_key(&model, key, control, shift, &result);
    }
    if (result == MessageComposerResult::changed) application_message_forward_edit_changed(revision != model.text_revision);
    return true;
}

bool application_message_forward_filter(const WindowEvent& event) noexcept {
    if (!input_plain_key(event)) return false;
    return ui_input_consume_key_tail(&g_app.message_forward_held_key,
        {event.type == EventType::key_down ? UiInputKind::key_down : UiInputKind::key_up, event.key.key,
         input_key_was_down(event)});
}
bool application_message_forward_window_event(const WindowEvent& event) noexcept {
    auto* q = g_app.message_forward;
    if (input_press(event) && (!q || !q->active)) g_app.message_forward_pointer_tail = false;
    if (input_release(event) && g_app.message_forward_pointer_tail && (!q || !q->active)) { g_app.message_forward_pointer_tail = false; return true; }
    if (!q || !q->active) return false;
    if (!available() || !quick_switcher_scope_equal(q->controller.scope, scope())) {
        if (input_key_down(event)) g_app.message_forward_held_key = event.key.key;
        application_message_forward_close(); return true;
    }
    if (event.type == EventType::cursor_query) {
        UiPoint point{};
        if (!client_cursor_dip(event, &point)) return false;
        application_set_cursor(ui_message_forward_scene_cursor(q->scene, point.x, point.y));
        return true;
    }
    if (input_focus_lost(event) || event.type == EventType::capture_lost || event.type == EventType::cancel_input) {
        revoke_capture(*q); q->blink_at = 0; q->caret_visible = false; ++q->content_revision;
        application_request_frame(); return false;
    }
    if (event.type == EventType::focus_changed) { caret_reset(*q); ++q->content_revision; application_request_frame(); return false; }
    if (event.type == EventType::context_menu || input_side_button(event) || input_double(event) ||
        input_horizontal_wheel(event)) return true;
    if (!input_press(event) && !input_release(event) && event.type != EventType::pointer_move && !input_vertical_wheel(event)) return false;
    if (input_vertical_wheel(event)) {
        ui_message_forward_scene_scroll(&q->scene, q->scene.scroll_dips -
            event.wheel.dy120 / 120.0f * kUiMessageForwardRowHeight * 3);
        application_request_frame(); return true;
    }
    const float x = client_dip_x(event), y = client_dip_y(event);
    if (event.type == EventType::pointer_move) {
        if (q->dragging && input_held(event)) pointer_edit(*q, x, y, true);
        else { bool changed = false; ui_message_forward_scene_set_hover(&q->scene, x, y, &changed); if (changed) application_request_frame(); }
        return true;
    }
    const auto hit = ui_message_forward_scene_hit_test(q->scene, x, y);
    if (input_press(event)) {
        g_app.message_forward_pointer_tail = false;
        q->pressed = hit; q->captured = true;
        q->dragging = !q->controller.committed && (hit.action == UiMessageForwardAction::edit || hit.action == UiMessageForwardAction::caption);
        if (q->dragging) { focus_field(*q, hit.action == UiMessageForwardAction::caption); pointer_edit(*q, x, y, input_shift(event)); }
        if (g_app.window) window_capture_pointer(g_app.window);
        return true;
    }
    const auto pressed = q->pressed; const bool captured = q->captured;
    revoke_capture(*q); g_app.message_forward_pointer_tail = false;
    if (captured && pressed.action == hit.action && pressed.channel_id == hit.channel_id &&
        pressed.source_revision == hit.source_revision && pressed.index == hit.index) activate(*q, hit);
    return true;
}
u64 application_message_forward_caret_wake() noexcept {
    return application_message_forward_edit_focused() && application_window_keyboard_focus() ? g_app.message_forward->blink_at : 0;
}
void application_message_forward_tick(u64 now) noexcept {
    auto* q = g_app.message_forward;
    if (!q || !deadline_due(now, application_message_forward_caret_wake())) return;
    q->caret_visible = !q->caret_visible; ++q->content_revision;
    q->blink_at = blink_deadline(now);
    application_request_frame();
}
void application_message_forward_completion(const MessageSendCompletion& completion) noexcept {
    auto* q = g_app.message_forward;
    if (!q || !quick_switcher_scope_equal(q->controller.scope, scope())) return;
    if (message_forward_observe_completion(&q->controller, completion)) {
        ++q->content_revision;
        if (q->active) application_request_frame();
    }
}
} // namespace maicord::app_detail
