#include "plugins.hpp"
#include "../shell/state.hpp"
#include "../shell/toast.hpp"
#include "../shell/content_input.hpp"
#include "../account/account_presentation.hpp"
#include "../account/profile.hpp"
#include "../chat/application_link_trust.hpp"
#include "../chat/composer/text_form.hpp"
#include "../chat/composer/text_form_state.hpp"
#include "maicord/core/memory.hpp"
#include "../account/account_ui_runtime.hpp"
#include "maicord/app/chat/messages/send_response.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"

// The application's side of the plugin UI: modals through the text form, banners through the
// conversation notice card (unread_banner.cpp asks plugin_banner_current), links through the link
// trust confirmation, and the clipboard and navigation actions.
namespace maicord::app_detail {
namespace {

// The form's answer is kept until the form has closed, so the plugin runs with the dialog gone.
struct Answer {
    bool accepted, toggle;
    u32 select, text_bytes;
    char text[kApplicationTextFormBytes];
};
Answer g_answer;
u64 g_serial;

bool modal_valid(TextFormOwnerKey key) noexcept {
    return plugin_modal_current() && key.item == g_serial && key.scope == g_app.plugin_account_scope_serial;
}
bool save_text(TextFormOwnerKey, Utf8View text) noexcept {
    g_answer.accepted = true;
    g_answer.text_bytes = minimum<u32>(text.bytes, sizeof(g_answer.text));
    copy_bytes(g_answer.text, text.data, g_answer.text_bytes);
    return true;
}
bool save_option(TextFormOwnerKey, bool checked) noexcept {
    g_answer.accepted = true;
    g_answer.toggle = checked;
    return true;
}
bool save_choices(TextFormOwnerKey key, Utf8View text, bool, u32, u32 select) noexcept {
    g_answer.select = select;
    return save_text(key, text);
}
void modal_closed(TextFormOwnerKey key) noexcept {
    if (key.item != g_serial) return;
    plugin_modal_resolve(g_answer.accepted, {g_answer.text, g_answer.text_bytes}, g_answer.toggle, g_answer.select);
    // What the user typed does not stay behind.
    secure_clear_bytes(&g_answer, sizeof(g_answer));
}

} // namespace
} // namespace maicord::app_detail

namespace maicord {
using namespace app_detail;

bool plugin_host_open_modal(const PluginModalRequest& modal) noexcept {
    if (!plugin_account_ready() || g_app.text_form) return false;
    g_answer = {};
    ++g_serial;
    ApplicationTextFormRequest request{};
    request.title = modal.title;
    request.context = modal.context;
    request.accept = modal.accept.bytes ? modal.accept : modal.single ? utf8_literal("Close") : utf8_literal("OK");
    request.single_action = modal.single;
    request.cancel = modal.cancel;
    request.danger = modal.danger;
    request.owner = {g_app.plugin_account_scope_serial, modal.owner_id, g_serial};
    request.valid = modal_valid;
    request.closed = modal_closed;
    switch (modal.kind) {
    case PluginModalKind::confirm:
        request.confirmation_only = true;
        request.accept_focused = true;
        request.save = save_text;
        break;
    case PluginModalKind::text:
        // Read-only text: a confirmation that shows it, with no editor, caret or text store.
        request.confirmation_only = true;
        request.accept_focused = true;
        request.label = modal.input_label;
        request.detail = modal.input_initial;
        request.save = save_text;
        break;
    case PluginModalKind::toggle:
        request.confirmation_only = true;
        request.accept_focused = true;
        request.option = modal.toggle_label;
        request.option_checked = modal.toggle_initial;
        request.save_option = save_option;
        break;
    case PluginModalKind::input_select:
        request.select_label = modal.select_label;
        request.select_options = {modal.select_items, modal.select_count};
        request.select_selected = modal.select_initial;
        request.save_choices = save_choices;
        [[fallthrough]];
    case PluginModalKind::input:
        request.label = modal.input_label;
        request.initial = modal.input_initial;
        request.placeholder = modal.input_hint;
        request.maximum_codepoints = modal.input_max_chars ? modal.input_max_chars : 2000;
        request.single_line = !modal.multiline;
        if (!request.save_choices) request.save = save_text;
        break;
    }
    return application_text_form_open(request);
}

void plugin_host_close_modal() noexcept {
    application_text_form_cancel_owner(modal_valid);
}

void plugin_host_banner_changed() noexcept {
    application_request_frame();
}

PluginUiStatus plugin_host_open_url(Utf8View url) noexcept {
    if (!plugin_account_ready()) return PluginUiStatus::busy;
    // Not trusted text: a host the user hasn't trusted asks first.
    return application_link_open(url, false) ? PluginUiStatus::ok : PluginUiStatus::denied;
}

PluginUiStatus plugin_host_copy_text(Utf8View text) noexcept {
    return application_copy_text(text, utf8_literal("Copied"), kToastCopyFailed) ? PluginUiStatus::ok
                                                                                  : PluginUiStatus::busy;
}

PluginUiStatus plugin_host_open_channel(u64 channel_id) noexcept {
    if (!plugin_account_ready()) return PluginUiStatus::busy;
    if (!chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id)) return PluginUiStatus::not_found;
    return application_navigate_known_message(channel_id, 0) ? PluginUiStatus::ok : PluginUiStatus::denied;
}

PluginUiStatus plugin_host_open_profile(u64 user_id, u64) noexcept {
    if (!plugin_account_ready()) return PluginUiStatus::busy;
    // No on-screen source: the card opens at the middle of the window.
    const UiRect viewport = application_viewport_dips();
    const float x = (viewport.left + viewport.right) * 0.5f, y = (viewport.top + viewport.bottom) * 0.5f;
    return application_profile_open(user_id, {x - 1.0f, y - 1.0f, x + 1.0f, y + 1.0f}, ProfileAnchor::point)
               ? PluginUiStatus::ok
               : PluginUiStatus::busy;
}

void plugin_host_toast(u64, Utf8View text, u32 tone) noexcept {
    application_toast_show(tone == 2 ? UiToastKind::success : tone == 4 ? UiToastKind::failure : UiToastKind::message,
                           text);
}

bool plugin_host_local_notice(u64 channel_id, Utf8View text, u64* message_id) noexcept {
    if (!g_app.account_ui || !text.bytes) return false;
    if (!channel_id) channel_id = g_app.account_ui->active_channel_id;
    if (!channel_id || g_app.account_ui->active_channel_id != channel_id) return false;
    // Local notice ids share one 22-bit sequence space: send refusals count up from zero and the
    // built-in commands use the upper half; plugins take the quarter between. The id's low bit is
    // always set, so the sequence steps by two.
    static u32 counter;
    const u32 sequence = 0x1C0000u | ((++counter << 1) & 0x3FFFEu);
    auto input = message_send_rejection_notice_input(channel_id, text, clock_unix_milliseconds(), sequence);
    input.author_id = 1;
    if (!message_timeline_insert_local(&g_app.message_view.timeline, input).ok()) {
        log_write(LogLevel::warning, "plugins", "local_notice_insert_failed");
        return false;
    }
    if (message_id) *message_id = input.id;
    refresh_account_frame();
    return true;
}

} // namespace maicord
