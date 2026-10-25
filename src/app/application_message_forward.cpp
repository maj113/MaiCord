#include "application_message_forward.hpp"
#include "maicord/core/memory.hpp"
#include "application_content_point.hpp"
#include "text_edit_keys.hpp"
#include "application_cursor.hpp"
#include "maicord/ui/scene_cursor.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_composer_text_input.hpp"
#include "application_friends.hpp"
#include "application_media_picker.hpp"
#include "application_search.hpp"
#include "application_pinned_messages.hpp"
#include "application_profile.hpp"
#include "application_message_interaction.hpp"
#include "application_account_menu.hpp"
#include "application_surface_motion.hpp"
#include "application_theme_editor.hpp"
#include "application_quick_switcher.hpp"
#include "application_video.hpp"
#include "maicord/app/message_forward_controller.hpp"
#include "maicord/protocol/message_card_excerpt.hpp"
#include "maicord/ui/message_forward_scene.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/core/deadline.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/winsock.hpp"

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
// Control characters and line separators never enter the single-line fields.
bool forbidden_scalar(u32 c) noexcept {
    return c < 32 || (c >= 127 && c <= 159) || c == 0x2028 || c == 0x2029;
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
    if (!out || !source_current(q) || !quick_switcher_resolve(q->controller.candidates,
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
    if (GetCapture() == g_app.window) ReleaseCapture();
}
void caret_reset(ApplicationMessageForward& q) noexcept {
    const u32 interval = GetCaretBlinkTime();
    q.caret_visible = true;
    q.blink_at = interval && interval != INFINITE ? deadline_after(clock_monotonic_milliseconds(), interval) : 0;
}
TextInputTarget delegate(const TextInputTarget& target) noexcept {
    return message_composer_input_target(static_cast<MessageComposer*>(target.context));
}
bool edit_ready(const TextInputTarget& target) noexcept {
    const auto* q = g_app.message_forward;
    return q && q->active && q->focused && !q->controller.committed && available() && quick_switcher_scope_equal(q->controller.scope, scope()) &&
        target.context == (q->caption_focused ? &q->caption : &q->edit) && target.identity == q->identity && text_input_ready(delegate(target));
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
        if (forbidden_scalar(in[i]))
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
        if (g_app.window) PostMessageW(g_app.window, WM_NULL, 0, 0);
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
    return model ? TextInputTarget{model, &edit_operations, g_app.message_forward->identity} : TextInputTarget{};
}
UiRect application_message_forward_edit_bounds() noexcept {
    auto* q = g_app.message_forward;
    return q && q->active ? (q->caption_focused ? q->scene.caption_text : q->scene.text) : UiRect{};
}
void application_message_forward_edit_cancel_native_group(const TextInputTarget& t) noexcept {
    const auto* q = g_app.message_forward;
    if (q && (t.context == &q->edit || t.context == &q->caption) && t.operations == &edit_operations && t.identity == q->identity) {
        text_input_end_group(delegate(t)); text_input_cancel(delegate(t));
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
    SecureZeroMemory(q, sizeof(*q)); VirtualFree(q, 0, MEM_RELEASE);
}
bool application_message_forward_synchronize() noexcept {
    auto* q = g_app.message_forward;
    if (!q) return true;
    if (!g_app.account_ui) { application_message_forward_close(); return true; }
    if (!quick_switcher_scope_equal(q->controller.scope, scope())) {
        // Never apply an old account's nonces to another account's outbox.
        if (q->account_ui == g_app.account_ui && q->account_instance == g_app.account.instance &&
            q->controller.scope.profile_key == scope().profile_key && q->controller.scope.self_user_id == scope().self_user_id)
            message_forward_cancel_pending(&q->controller, &g_app.account_ui->message_composer_controller);
        application_message_forward_close();
        reset_value(&q->controller);
        message_composer_reset(&q->edit, 0, false); message_composer_reset(&q->caption, 0, false);
        return true;
    }
    const u32 previous = q->controller.candidates.revision;
    message_forward_synchronize(&q->controller, scope(), sources(), message_composer_text(q->edit),
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
    for (u32 i = 0; i < q->controller.destination_count; ++i) {
        const auto& d = q->controller.destinations[i];
        if (!q->controller.committed && d.state != MessageForwardDestinationState::ready) q->content.can_send = false;
        q->content.selected[i] = {d.row.target.channel_id, {d.row.label, d.row.label_bytes}, message_forward_destination_status(d),
            d.state == MessageForwardDestinationState::sent ? UiMessageForwardState::sent :
            d.state == MessageForwardDestinationState::cancelled ? UiMessageForwardState::cancelled :
            (d.state == MessageForwardDestinationState::failed || d.state == MessageForwardDestinationState::unavailable) ? UiMessageForwardState::failed :
            d.state == MessageForwardDestinationState::ready ? UiMessageForwardState::ready : UiMessageForwardState::sending};
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
        if (GetCapture() == g_app.window) ReleaseCapture();
        q->active = true;
        if (g_app.window) SetFocus(g_app.window);
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
        q = static_cast<ApplicationMessageForward*>(VirtualAlloc(nullptr, sizeof(*q), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!q) return false;
        if (!message_composer_initialize(&q->edit, {q->query, sizeof(q->query)}) ||
            !message_composer_initialize(&q->caption, {q->caption_text, sizeof(q->caption_text)})) {
            message_composer_release(&q->edit); message_composer_release(&q->caption); VirtualFree(q, 0, MEM_RELEASE); return false;
        }
        g_app.message_forward = q;
    }
    if (g_app.message_forward_lifetime_serial == ~u64{0}) return false;
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
    if (GetCapture() == g_app.window) ReleaseCapture();
    if (!application_message_forward_synchronize()) { application_message_forward_close(); return false; }
    if (g_app.window) SetFocus(g_app.window);
    return application_composer_text_input_synchronize() && application_request_frame();
}
bool application_message_forward_edit_changed(bool content) noexcept {
    auto* q = g_app.message_forward;
    if (!application_message_forward_edit_focused()) return true;
    if (content && q->caption_focused) message_forward_set_caption(&q->controller, message_composer_text(q->caption));
    ++q->content_revision; caret_reset(*q);
    return application_message_forward_synchronize() && application_request_frame();
}
bool application_message_forward_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    if (ui_input_consume_key_tail(&g_app.message_forward_held_key, {kind, key, repeated})) return true;
    auto* q = g_app.message_forward;
    if (!q || !q->active) return false;
    if (!available() || !quick_switcher_scope_equal(q->controller.scope, scope())) {
        if (kind == UiInputKind::key_down || kind == UiInputKind::system_key_down)
            g_app.message_forward_held_key = key;
        application_message_forward_close(); return true;
    }
    const bool ctrl_raw = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool control = ctrl_raw && !alt, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (kind == UiInputKind::key_up) return true;
    if (kind == UiInputKind::key_down && key == VK_ESCAPE) { g_app.message_forward_held_key = key; application_message_forward_close(); return true; }
    if (kind == UiInputKind::key_down && key == VK_TAB) {
        g_app.message_forward_held_key = key; if (!repeated) focus_field(*q, !q->caption_focused); return true;
    }
    if (kind == UiInputKind::key_down && key == VK_RETURN) {
        g_app.message_forward_held_key = key;
        if (!repeated && !application_composer_text_input_composing()) {
            if (control || q->caption_focused) activate(*q, {q->controller.committed ? UiMessageForwardAction::retry : UiMessageForwardAction::send});
            else activate(*q, ui_message_forward_scene_selected(q->scene));
        }
        return true;
    }
    if (kind == UiInputKind::key_down && (key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT)) {
        ui_message_forward_scene_move(&q->scene, key == VK_UP ? UiQuickSwitcherMove::previous :
            key == VK_DOWN ? UiQuickSwitcherMove::next : key == VK_PRIOR ? UiQuickSwitcherMove::page_previous : UiQuickSwitcherMove::page_next);
        application_request_frame(); return true;
    }
    if (!application_message_forward_edit_focused()) return true;
    auto& model = q->caption_focused ? q->caption : q->edit;
    if (kind == UiInputKind::character) {
        if (control || (alt && !ctrl_raw) || forbidden_scalar(key) || key > 0xffff) return true;
        if (!model.history) message_composer_enable_history(&model, 2048);
        if (message_composer_insert_utf16(&model, static_cast<u16>(key), clock_monotonic_milliseconds()) == MessageComposerResult::changed)
            application_message_forward_edit_changed(true);
        return true;
    }
    if (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down) return true;
    const auto revision = model.text_revision;
    MessageComposerResult result = MessageComposerResult::unchanged;
    if (control && (key == 'C' || key == 'X')) {
        const u32 a = minimum(model.anchor, model.caret), b = maximum(model.anchor, model.caret);
        if (b > a && clipboard_write_utf8(g_app.window, {model.text + a, b - a}) && key == 'X')
            result = message_composer_replace_range(&model, a, b, {});
    } else if (control && key == 'V') {
        char text[kMessageForwardCaptionMaximumBytes];
        const auto read = clipboard_read_utf8(g_app.window, {text, sizeof(text)});
        bool valid = read.ok();
        for (u32 i = 0; valid && i < read.bytes;) {
            const auto scalar = utf8_decode_one(text + i, read.bytes - i);
            valid = scalar.valid && !forbidden_scalar(scalar.codepoint);
            if (valid) i += scalar.width;
        }
        if (valid) result = message_composer_insert_utf8(&model, {text, read.bytes});
    } else {
        if (key == VK_BACK) g_app.message_forward_held_key = key;
        message_composer_edit_key(&model, key, control, shift, &result);
    }
    if (result == MessageComposerResult::changed) application_message_forward_edit_changed(revision != model.text_revision);
    return true;
}

bool application_message_forward_filter(const MSG& message) noexcept {
    if (message.hwnd != g_app.window || (message.message != WM_KEYDOWN && message.message != WM_KEYUP)) return false;
    return ui_input_consume_key_tail(&g_app.message_forward_held_key,
        {message.message == WM_KEYDOWN ? UiInputKind::key_down : UiInputKind::key_up,
         static_cast<u32>(message.wParam), (message.lParam & (1ll << 30)) != 0});
}
bool application_message_forward_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* q = g_app.message_forward;
    if (message == WM_LBUTTONDOWN && (!q || !q->active)) g_app.message_forward_pointer_tail = false;
    if (message == WM_LBUTTONUP && g_app.message_forward_pointer_tail && (!q || !q->active)) { g_app.message_forward_pointer_tail = false; return true; }
    if (!q || !q->active) return false;
    if (!available() || !quick_switcher_scope_equal(q->controller.scope, scope())) {
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) g_app.message_forward_held_key = static_cast<u32>(wparam);
        application_message_forward_close(); return true;
    }
    if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) {
        UiPoint point{};
        if (!application_cursor_point(g_app.window, g_app.renderer.dpi, lparam, &point)) return false;
        application_set_cursor(ui_message_forward_scene_cursor(q->scene, point.x, point.y));
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
    if (message == WM_MOUSEWHEEL) application_screen_to_content(&point);
    const float scale = 96.f / maximum(1u, g_app.renderer.dpi), x = point.x * scale, y = point.y * scale;
    if (message == WM_MOUSEWHEEL) {
        ui_message_forward_scene_scroll(&q->scene, q->scene.scroll_dips -
            static_cast<short>(HIWORD(wparam)) / static_cast<float>(WHEEL_DELTA) * kUiMessageForwardRowHeight * 3);
        application_request_frame(); return true;
    }
    if (message == WM_MOUSEMOVE) {
        if (q->dragging && (wparam & MK_LBUTTON)) pointer_edit(*q, x, y, true);
        else { bool changed = false; ui_message_forward_scene_set_hover(&q->scene, x, y, &changed); if (changed) application_request_frame(); }
        return true;
    }
    const auto hit = ui_message_forward_scene_hit_test(q->scene, x, y);
    if (message == WM_LBUTTONDOWN) {
        g_app.message_forward_pointer_tail = false;
        q->pressed = hit; q->captured = true;
        q->dragging = !q->controller.committed && (hit.action == UiMessageForwardAction::edit || hit.action == UiMessageForwardAction::caption);
        if (q->dragging) { focus_field(*q, hit.action == UiMessageForwardAction::caption); pointer_edit(*q, x, y, (GetKeyState(VK_SHIFT) & 0x8000) != 0); }
        if (g_app.window) SetCapture(g_app.window);
        return true;
    }
    const auto pressed = q->pressed; const bool captured = q->captured;
    revoke_capture(*q); g_app.message_forward_pointer_tail = false;
    if (captured && pressed.action == hit.action && pressed.channel_id == hit.channel_id &&
        pressed.source_revision == hit.source_revision && pressed.index == hit.index) activate(*q, hit);
    return true;
}
u64 application_message_forward_caret_wake() noexcept {
    return application_message_forward_edit_focused() && GetFocus() == g_app.window ? g_app.message_forward->blink_at : 0;
}
void application_message_forward_tick(u64 now) noexcept {
    auto* q = g_app.message_forward;
    if (!q || !deadline_due(now, application_message_forward_caret_wake())) return;
    q->caret_visible = !q->caret_visible; ++q->content_revision;
    const u32 interval = GetCaretBlinkTime();
    q->blink_at = interval && interval != INFINITE ? deadline_after(now, interval) : 0;
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
u32 application_message_forward_owner_bytes() noexcept { return g_app.message_forward ? sizeof(ApplicationMessageForward) : 0; }
} // namespace maicord::app_detail
