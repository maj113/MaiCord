#include "application_pinned_messages.hpp"
#include "maicord/app/shortcut_registry.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_composer_text_input.hpp"
#include "application_content_input.hpp"
#include "application_friends.hpp"
#include "application_retained_projection.hpp"
#include "application_search.hpp"
#include "application_settings.hpp"
#include "application_text_form.hpp"
#include "rest_rate_retry.hpp"
#include "maicord/app/pinned_message_controller.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/timestamp_format.hpp"

#include <Windows.h>

namespace maicord::app_detail {
struct ApplicationPinnedMessages {
    PinnedMessageController controller;
    PinnedMessageRequest submitted;
    PinnedMessageScope pressed_scope;
    MessagePin decoded[kMessagePinsMaximumItems];
    UiPinnedMessagesScene scene;
    UiPinnedMessagesSource source;
    UiPinnedMessagesContent content;
    UiPinnedMessagesTarget pressed;
    char timestamps[kMessagePinsMaximumItems][128];
    char details[kMessagePinsMaximumItems][192];
    u16 timestamp_bytes[kMessagePinsMaximumItems], detail_bytes[kMessagePinsMaximumItems];
    float body_heights[kMessagePinsMaximumItems];
    char channel[160];
    u16 channel_bytes;
    u64 projected_revision, pressed_source_revision;
    u32 permission_revision, heading_revision, held_key;
    float measured_width, measured_em, measured_tracking, thumb_grab;
    const void* measured_native_text;
    u32 measured_dpi, measured_native_format;
    TextFormOwnerKey confirmation;
    bool panel, captured, scrollbar_dragging, submitted_cancelled, confirming, confirmation_pin;
};
static_assert(sizeof(ApplicationPinnedMessages) <= 128 * 1024);
namespace {
PinnedMessageScope scope() noexcept {
    if (!g_app.account_ui) return {};
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, g_app.account_ui->active_channel_id);
    return channel && channel->occupied
        ? PinnedMessageScope{g_app.plugin_account_scope_serial, g_app.account.status.profile_id,
                             state.self_user_id, channel->id, channel->generation} : PinnedMessageScope{};
}
const ChannelRecord* selected_channel() noexcept {
    return g_app.account_ui ? chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                               g_app.account_ui->active_channel_id) : nullptr;
}
bool pin_permission() noexcept {
    const auto* channel = selected_channel();
    return application_pinned_messages_available() && channel &&
           (channel->actions & channel_action_pin_messages);
}
bool current(const ApplicationPinnedMessages& value) noexcept {
    return value.controller.active && application_pinned_messages_available() &&
           pinned_message_scope_equal(value.controller.scope, scope());
}
bool source_current(void* context) noexcept {
    return context && current(*static_cast<ApplicationPinnedMessages*>(context)) &&
           static_cast<ApplicationPinnedMessages*>(context)->panel;
}
bool front() noexcept {
    return application_pinned_messages_available() && !application_settings_active() && !g_app.text_form &&
           !g_app.popup_scene.active && !g_app.image_viewer.scene.active && !g_app.reaction_viewer.active &&
           !(g_app.renderer.profile_scene && g_app.renderer.profile_scene->active);
}
ApplicationPinnedMessages* ensure_owner() noexcept {
    if (!g_app.pinned_messages) {
        g_app.pinned_messages = static_cast<ApplicationPinnedMessages*>(
            VirtualAlloc(nullptr, sizeof(ApplicationPinnedMessages), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (g_app.pinned_messages) {
            g_app.pinned_messages->source = {g_app.pinned_messages, 0, 1, nullptr, source_current};
        }
    }
    return g_app.pinned_messages;
}
void cancel_submitted(ApplicationPinnedMessages& value) noexcept {
    if (value.controller.request_id && !value.submitted_cancelled && g_app.account.instance &&
        g_app.account.api.pinned_messages) {
        g_app.account.api.pinned_messages(g_app.account.instance, nullptr, value.controller.request_id);
        value.submitted_cancelled = true;
    }
}
void clear_capture(ApplicationPinnedMessages& value) noexcept {
    value.pressed = {};
    value.pressed_scope = {};
    value.scrollbar_dragging = false;
    const bool captured = value.captured;
    value.captured = false;
    if (captured && GetCapture() == g_app.window) ReleaseCapture();
}
bool repaint() noexcept {
    return application_pinned_messages_synchronize() && refresh_retained_projection();
}
Utf8View row_body(const PinnedMessageRow& row) noexcept {
    const auto text = pinned_message_content(row);
    if (text.bytes) return text;
    if (row.forwarded) return utf8_literal("Forwarded message");
    if (row.attachment_count) return utf8_literal("Attachment");
    if (row.sticker_count) return utf8_literal("Sticker");
    if (row.embed_count) return utf8_literal("Embedded content");
    return utf8_literal("Message has no text");
}
bool resolve(void* context, u32 index, UiPinnedMessagesRow* output) noexcept {
    auto& value = *static_cast<ApplicationPinnedMessages*>(context);
    const auto* row = pinned_message_controller_row(value.controller, index);
    if (!output || !row) return false;
    const auto author = pinned_message_author(*row);
    const bool pending = pinned_message_controller_mutation_pending(value.controller) &&
        value.controller.pending.message_id == row->message_id;
    *output = {row->channel_id, row->message_id, row->author_id,
        author.bytes ? author : utf8_literal("Unknown user"),
        {value.timestamps[index], value.timestamp_bytes[index]}, row_body(*row),
        {value.details[index], value.detail_bytes[index]}, pinned_message_avatar(*row),
        value.body_heights[index], pending || (row->type != 6 && value.controller.authoritative && pin_permission() &&
            !value.controller.permission_denied && !value.controller.request_id && !pinned_message_controller_mutation_pending(value.controller)),
        pending, value.controller.authoritative};
    return true;
}
void prepare_rows(ApplicationPinnedMessages& value) noexcept {
    const auto now = clock_unix_milliseconds() / 1000;
    for (u32 index = 0; index < value.controller.page.count; ++index) {
        const auto& row = value.controller.rows[index];
        const auto date = format_timestamp_local(row.timestamp_ms / 1000, 'f', now,
            {value.timestamps[index], sizeof(value.timestamps[index])});
        value.timestamp_bytes[index] = date.ok ? static_cast<u16>(date.bytes) : 0;
        TextBank detail{value.details[index], sizeof(value.details[index]), 0};
        detail.add(utf8_literal("Pinned "));
        const auto pinned = format_timestamp_local(row.pinned_at_ms / 1000, 'd', now,
            {detail.data + detail.used, detail.capacity - detail.used});
        if (pinned.ok) detail.used += pinned.bytes;
        else detail.add(utf8_literal("message"));
        if (row.attachment_count) detail.add(utf8_literal(" · Attachments"));
        if (row.embed_count) detail.add(utf8_literal(" · Embeds"));
        if (row.sticker_count) detail.add(utf8_literal(" · Stickers"));
        if (row.forwarded) detail.add(utf8_literal(" · Forwarded"));
        if (row.excerpt_truncated) detail.add(utf8_literal(" · Open to read more"));
        value.detail_bytes[index] = static_cast<u16>(detail.used);
        value.body_heights[index] = 48.f;
    }
    value.measured_native_text = nullptr;
    value.measured_width = 0.f;
}
bool measure_rows(ApplicationPinnedMessages& value) noexcept {
    if (!value.controller.has_page || !ui_rect_has_area(value.scene.body)) return false;
    auto system = renderer_text_system(&g_app.renderer, UiTextFace::normal);
    if (!system.native_text || !system.native_format) return false;
    const float width = maximum(24.f, ui_rect_width(value.scene.body) - 60.f);
    if (value.measured_width == width && value.measured_em == system.em_size &&
        value.measured_tracking == system.tracking &&
        value.measured_native_text == system.native_text && value.measured_native_format == system.native_format &&
        value.measured_dpi == g_app.renderer.dpi) return false;
    wchar_t scratch[kPinnedMessageExcerptBytes];
    for (u32 index = 0; index < value.controller.page.count; ++index) {
        const auto measured = text_measure_utf8_styled(&system, row_body(value.controller.rows[index]), {},
            width, 144.f, {scratch, kPinnedMessageExcerptBytes}, TextWrapping::wrap);
        value.body_heights[index] = measured.ok() ? maximum(24.f, minimum(144.f, measured.height + 2.f)) : 48.f;
    }
    value.measured_width = width;
    value.measured_em = system.em_size;
    value.measured_tracking = system.tracking;
    value.measured_native_text = system.native_text;
    value.measured_native_format = system.native_format;
    value.measured_dpi = g_app.renderer.dpi;
    ++value.source.revision;
    return true;
}
Utf8View status(const PinnedMessageController& controller) noexcept {
    if (controller.failure == PinnedMessageFailure::permissions)
        return controller.read_denied || !controller.available
            ? utf8_literal("You no longer have permission to access these pins.")
            : utf8_literal("You no longer have permission to pin messages.");
    if (controller.failure == PinnedMessageFailure::rate_limited)
        return utf8_literal("Try again in a moment.");
    if (controller.phase == PinnedMessagePhase::mutating)
        return controller.pending.operation == MessagePinsOperation::pin
            ? utf8_literal("Pinning message…") : utf8_literal("Unpinning message…");
    if (controller.phase == PinnedMessagePhase::loading)
        return controller.has_page ? utf8_literal("Updating pinned messages…") : utf8_literal("Loading pinned messages…");
    if (controller.failure != PinnedMessageFailure::none)
        return utf8_literal("Pinned messages could not be updated. Try again.");
    return {};
}
bool confirmation_valid(TextFormOwnerKey key) noexcept {
    const auto* value = g_app.pinned_messages;
    return value && value->confirming && key.scope == value->confirmation.scope &&
        key.collection == value->confirmation.collection && key.item == value->confirmation.item &&
        current(*value) && value->controller.scope.account_scope == key.scope &&
        value->controller.scope.channel_id == key.collection && application_pinned_messages_can_pin(key.item);
}
bool confirmation_save(TextFormOwnerKey key, Utf8View) noexcept {
    if (!confirmation_valid(key)) return false;
    auto& value = *g_app.pinned_messages;
    value.confirming = false;
    if (!pinned_message_controller_mutate(&value.controller, key.item, value.confirmation_pin)) return false;
    return repaint();
}
bool activate(UiPinnedMessagesTarget target) noexcept {
    auto* value = g_app.pinned_messages;
    if (!value || !current(*value) || target.channel_id != value->controller.scope.channel_id) return false;
    switch (target.action) {
    case UiPinnedMessagesAction::close: application_pinned_messages_close(); return repaint();
    case UiPinnedMessagesAction::refresh:
        if (value->controller.read_denied && !value->controller.request_id)
            pinned_message_controller_open(&value->controller, scope(), application_pinned_messages_available(), pin_permission());
        else pinned_message_controller_refresh(&value->controller);
        break;
    case UiPinnedMessagesAction::previous: pinned_message_controller_previous(&value->controller); break;
    case UiPinnedMessagesAction::next: pinned_message_controller_next(&value->controller); break;
    case UiPinnedMessagesAction::retry:
        if (value->controller.read_denied && !value->controller.request_id)
            pinned_message_controller_open(&value->controller, scope(), application_pinned_messages_available(), pin_permission());
        else if (value->controller.permission_denied) pinned_message_controller_refresh(&value->controller);
        else pinned_message_controller_retry(&value->controller);
        break;
    case UiPinnedMessagesAction::unpin:
        return application_pinned_messages_action(target.message_id, false, (GetKeyState(VK_SHIFT) & 0x8000) == 0);
    case UiPinnedMessagesAction::jump:
        if (!pinned_message_controller_select(&value->controller, target.message_id)) return true;
        if (value->scene.overlay) application_pinned_messages_close();
        return application_navigate_notification(target.channel_id, target.message_id);
    default: return true;
    }
    return repaint();
}
} // namespace

bool application_pinned_messages_available() noexcept {
    const auto* channel = selected_channel();
    constexpr u32 required = channel_action_view | channel_action_read_history;
    return channel && channel->occupied && !g_app.login.visible && !application_friends_active() &&
        channel_is_message_capable(channel->type) && !(channel->flags & kChannelObfuscatedFlag) &&
        channel->permissions_resolved && (channel->actions & required) == required &&
        g_app.plugin_account_scope_serial && g_app.account.status.profile_id &&
        g_app.account_ui->chat_state.self_user_id &&
        g_app.account_ui->chat_state.selected_channel_id == channel->id &&
        g_app.account_ui->chat_state.selected_guild_id == channel->guild_id;
}
bool application_pinned_messages_active() noexcept {
    return g_app.pinned_messages && g_app.pinned_messages->panel && current(*g_app.pinned_messages);
}
bool application_pinned_messages_read_blocked() noexcept {
    return application_pinned_messages_active() && g_app.pinned_messages->scene.overlay;
}
bool application_pinned_messages_can_pin(u64 message_id) noexcept {
    if (!message_id || !pin_permission()) return false;
    const auto* value = g_app.pinned_messages;
    if (value && current(*value) && (!value->controller.available || !value->controller.can_pin ||
        value->controller.permission_denied || value->controller.request_id ||
        pinned_message_controller_mutation_pending(value->controller))) return false;
    const auto found = message_store_find(g_app.message_view.store, message_id);
    const auto* record = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (record) return record->channel_id == scope().channel_id &&
        record->delivery == MessageDeliveryState::authoritative && record->type != 6;
    if (value && current(*value) && value->controller.authoritative)
        for (u32 index = 0; index < value->controller.page.count; ++index)
            if (value->controller.rows[index].message_id == message_id && value->controller.rows[index].type != 6) return true;
    return false;
}
bool application_pinned_messages_toggle() noexcept {
    if (application_pinned_messages_active()) { application_pinned_messages_close(); return repaint(); }
    if (!application_pinned_messages_available()) return false;
    auto* value = ensure_owner();
    if (!value || value->controller.request_id ||
        !pinned_message_controller_open(&value->controller, scope(), true, pin_permission())) return false;
    if (!application_search_close_panel()) {
        pinned_message_controller_cancel(&value->controller);
        return false;
    }
    application_composer_text_input_suspend();
    g_app.message_composer.focused = false;
    g_app.message_action_bar = {};
    value->panel = true;
    value->permission_revision = g_app.account_ui->chat_state.permission_mutations;
    value->source.resolve = resolve;
    if (!repaint()) return false;
    g_app.context_focus = &value->scene.widgets;
    ui_pinned_messages_scene_focus(&value->scene, {UiPinnedMessagesAction::close, scope().channel_id, 0});
    return true;
}
void application_pinned_messages_close() noexcept {
    auto* value = g_app.pinned_messages;
    g_app.renderer.pinned_messages_scene = nullptr;
    if (!value) return;
    application_text_form_cancel_owner(confirmation_valid);
    value->confirming = false;
    value->panel = false;
    clear_capture(*value);
    cancel_submitted(*value);
    pinned_message_controller_cancel(&value->controller);
    if (g_app.context_focus == &value->scene.widgets) g_app.context_focus = nullptr;
    application_request_frame();
}
void application_pinned_messages_release() noexcept {
    application_pinned_messages_close();
    if (g_app.pinned_messages) {
        ui_pinned_messages_scene_release(&g_app.pinned_messages->scene);
        VirtualFree(g_app.pinned_messages, 0, MEM_RELEASE);
        g_app.pinned_messages = nullptr;
    }
}
bool application_pinned_messages_synchronize() noexcept {
    auto* value = g_app.pinned_messages;
    if (!value) { g_app.renderer.pinned_messages_scene = nullptr; return true; }
    if (value->confirming && !g_app.text_form) value->confirming = false;
    const auto old_active = value->controller.active;
    const auto* channel = selected_channel();
    if (channel && g_app.account_ui->chat_state.permission_mutations != value->permission_revision &&
        pinned_message_scope_equal(value->controller.scope, scope())) {
        value->permission_revision = g_app.account_ui->chat_state.permission_mutations;
        pinned_message_controller_reauthorize(&value->controller, scope(), application_pinned_messages_available(), pin_permission());
    } else pinned_message_controller_authority(&value->controller, scope(), application_pinned_messages_available(), pin_permission());
    if (old_active && !value->controller.active) {
        cancel_submitted(*value);
        value->panel = false;
        clear_capture(*value);
        application_text_form_cancel_owner(confirmation_valid);
        value->confirming = false;
        if (g_app.context_focus == &value->scene.widgets) g_app.context_focus = nullptr;
    }
    if (!value->panel || !current(*value)) {
        const bool header_press = value->captured && value->pressed.message_id == ~u64{0} &&
            front() && pinned_message_scope_equal(value->pressed_scope, scope());
        if (!header_press) clear_capture(*value);
        cancel_submitted(*value);
        if (!current(*value)) {
            application_text_form_cancel_owner(confirmation_valid);
            value->confirming = false;
        }
        if (g_app.context_focus == &value->scene.widgets) g_app.context_focus = nullptr;
        g_app.renderer.pinned_messages_scene = nullptr;
        return true;
    }
    if (!front()) clear_capture(*value);
    if (value->projected_revision != value->controller.revision) {
        value->projected_revision = value->controller.revision;
        ++value->source.revision;
        value->source.count = value->controller.has_page && value->controller.available ? value->controller.page.count : 0;
    }
    const auto name = channel ? chat_state_store_channel_name(g_app.account_ui->chat_state, *channel) : Utf8View{};
    u32 bytes = minimum(name.bytes, static_cast<u32>(sizeof(value->channel)));
    while (bytes < name.bytes && bytes && static_cast<u8>(name.data[bytes]) >= 0x80 &&
           static_cast<u8>(name.data[bytes]) < 0xc0) --bytes;
    if (bytes != value->channel_bytes || (bytes && !bytes_equal(value->channel, name.data, bytes))) {
        if (bytes) CopyMemory(value->channel, name.data, bytes);
        value->channel_bytes = static_cast<u16>(bytes);
        ++value->heading_revision;
    }
    const u32 revision = value->heading_revision + static_cast<u32>(value->controller.revision);
    const auto& controller = value->controller;
    value->content = {{value->channel, value->channel_bytes}, status(controller), controller.scope.channel_id,
        revision, controller.phase == PinnedMessagePhase::failed ? UiPinnedMessagesPhase::failed :
            controller.has_page ? UiPinnedMessagesPhase::ready : UiPinnedMessagesPhase::loading,
        controller.has_previous && controller.authoritative, controller.page.has_more && controller.authoritative,
        controller.phase == PinnedMessagePhase::loading && controller.has_page,
        controller.failure != PinnedMessageFailure::none && controller.has_page, controller.page_index};
    auto viewport = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::content);
    viewport.top = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::title_bar).bottom;
    viewport.bottom = minimum(viewport.bottom,
        ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::message_view).bottom);
    viewport.right = static_cast<float>(g_app.renderer.width) * 96.f / maximum(g_app.renderer.dpi, 1u);
    if (!ui_pinned_messages_scene_update(&value->scene, &value->source, &value->content, viewport)) return false;
    if (measure_rows(*value) && !ui_pinned_messages_scene_update(&value->scene, &value->source, &value->content, viewport)) return false;
    g_app.renderer.pinned_messages_scene = &value->scene;
    return true;
}
bool application_pinned_messages_pending(u64 now_ms) noexcept {
    return g_app.account.status.running && g_app.account.instance && g_app.account.api.pinned_messages &&
        g_app.pinned_messages && current(*g_app.pinned_messages) && !g_app.pinned_messages->confirming &&
        pinned_message_controller_request(g_app.pinned_messages->controller, now_ms).ready;
}
bool application_pinned_messages_submit(u64 request_id, u64 now_ms) noexcept {
    if (!application_pinned_messages_pending(now_ms) || !request_id || !g_app.account.instance || !g_app.account.api.pinned_messages) return false;
    auto& value = *g_app.pinned_messages;
    const auto request = pinned_message_controller_request(value.controller, now_ms);
    const auto admission = g_app.account.api.pinned_messages(g_app.account.instance, &request.payload, request_id);
    if (!admission.ok()) {
        pinned_message_controller_admission_failed(&value.controller, request, admission.error == RestClientSubmitError::busy);
        repaint(); return false;
    }
    if (!pinned_message_controller_submitted(&value.controller, request, request_id)) {
        g_app.account.api.pinned_messages(g_app.account.instance, nullptr, request_id); return false;
    }
    value.submitted = request;
    value.submitted_cancelled = false;
    repaint();
    return true;
}
bool application_pinned_messages_response(const RestClientResponse& response, Utf8View body) noexcept {
    auto* value = g_app.pinned_messages;
    if (!value || !response.request_id || response.request_id != value->controller.request_id) return false;
    if (!application_pinned_messages_synchronize()) return false;
    const auto submitted = value->submitted;
    PinnedMessageCompletion completion{PinnedMessageCompletionKind::rejected};
    MessagePinsDecodeResult decoded{};
    const auto now = clock_monotonic_milliseconds();
    if (const u64 retry_at = rest_rate_retry_deadline(response)) {
        completion = {PinnedMessageCompletionKind::rate_limited, retry_at};
    } else if (response.status_code == 429) completion.kind = PinnedMessageCompletionKind::rejected;
    else if (response.error == RestClientError::cancelled) completion.kind = PinnedMessageCompletionKind::cancelled;
    else if (!response.transport_ok() || response.status_code >= 500) completion.kind = PinnedMessageCompletionKind::ambiguous;
    else if (response.status_code == 403 || response.status_code == 401) completion.kind = PinnedMessageCompletionKind::permission_lost;
    else if (response.status_code >= 200 && response.status_code < 300) {
        if (submitted.payload.operation != MessagePinsOperation::fetch) completion.kind = PinnedMessageCompletionKind::success;
        else if (current(*value) && g_app.account_ui) {
            MessageHistoryDecodeScratch scratch{};
            if (application_message_decode_scratch(*g_app.account_ui, &scratch, body)) {
                decoded = decode_message_pins_page(body, submitted.payload.query, scratch.page_structurals,
                    scratch.message_structurals, scratch.maximum_depth, scratch.content,
                    {value->decoded, kMessagePinsMaximumItems}, scratch.mention_users, scratch.rich);
                if (decoded.ok()) completion.kind = PinnedMessageCompletionKind::success;
            }
        }
    }
    const bool remote_current = value->controller.remote_revision == value->controller.submitted_remote_revision;
    const bool consumed = pinned_message_controller_complete(&value->controller, submitted.scope, response.request_id,
        completion, decoded.ok() && submitted.payload.operation == MessagePinsOperation::fetch ? &decoded.page : nullptr,
        {value->decoded, decoded.ok() ? decoded.page.count : 0}, now);
    if (!consumed) return false;
    // Only a successful server response changes the loaded menu state.
    // Pin pages are a separate view and never replace the timeline's content.
    if (completion.kind == PinnedMessageCompletionKind::success && current(*value)) {
        if (submitted.payload.operation != MessagePinsOperation::fetch && remote_current) {
            const auto found = message_store_find(g_app.message_view.store, submitted.payload.message_id);
            const auto* record = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
            if (record && record->channel_id == submitted.scope.channel_id) {
                const u32 flags = submitted.payload.operation == MessagePinsOperation::pin
                    ? record->presentation_flags | message_presentation_pinned
                    : record->presentation_flags & ~message_presentation_pinned;
                message_store_set_presentation_flags(&g_app.message_view.store, record->id, flags);
            }
        } else if (value->controller.phase == PinnedMessagePhase::ready) {
            for (u32 index = 0; index < value->controller.page.count; ++index) {
                const auto found = message_store_find(g_app.message_view.store, value->controller.rows[index].message_id);
                const auto* record = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
                if (record && record->channel_id == submitted.scope.channel_id)
                    message_store_set_presentation_flags(&g_app.message_view.store, record->id,
                        record->presentation_flags | message_presentation_pinned);
            }
        }
    }
    value->submitted = {};
    value->submitted_cancelled = false;
    if (decoded.ok() && completion.kind == PinnedMessageCompletionKind::success &&
        value->controller.phase == PinnedMessagePhase::ready) {
        prepare_rows(*value);
    }
    return repaint();
}
u64 application_pinned_messages_next_wake() noexcept {
    return g_app.account.status.running && g_app.account.instance && g_app.account.api.pinned_messages &&
        g_app.pinned_messages && current(*g_app.pinned_messages) && !g_app.pinned_messages->confirming
        ? pinned_message_controller_next_wake(g_app.pinned_messages->controller) : 0;
}
bool application_pinned_messages_action(u64 message_id, bool pin, bool confirm) noexcept {
    if (!application_pinned_messages_can_pin(message_id)) return false;
    auto* value = ensure_owner();
    if (!value) return false;
    if (!current(*value) && !pinned_message_controller_open(&value->controller, scope(), true, pin_permission())) return false;
    value->panel = true;
    value->source.resolve = resolve;
    value->permission_revision = g_app.account_ui->chat_state.permission_mutations;
    if (!application_search_close_panel()) {
        pinned_message_controller_cancel(&value->controller);
        value->panel = false;
        return false;
    }
    application_composer_text_input_suspend();
    g_app.message_composer.focused = false;
    g_app.message_action_bar = {};
    value->confirmation = {value->controller.scope.account_scope, value->controller.scope.channel_id, message_id};
    value->confirmation_pin = pin;
    if (!confirm) return pinned_message_controller_mutate(&value->controller, message_id, pin) && repaint();
    value->confirming = true;
    const ApplicationTextFormRequest request{pin ? utf8_literal("Pin message?") : utf8_literal("Unpin message?"),
        pin ? utf8_literal("Keep this message in the channel’s pinned messages.") : utf8_literal("Remove this message from the channel’s pinned messages."),
        {}, pin ? utf8_literal("Pin message") : utf8_literal("Unpin message"), {},
        value->confirmation, confirmation_valid, confirmation_save, 0, true};
    if (!application_text_form_open(request)) { value->confirming = false; return false; }
    return repaint();
}
void application_pinned_messages_gateway(u64 channel_id) noexcept {
    auto* value = g_app.pinned_messages;
    if (value && current(*value) && value->controller.scope.channel_id == channel_id) {
        pinned_message_controller_invalidate(&value->controller, value->controller.scope);
        application_request_frame();
    }
}
bool application_pinned_messages_scroll_range(UiScrollRange* output) noexcept {
    if (!output || !application_pinned_messages_active() || !front()) return false;
    const auto& value = *g_app.pinned_messages;
    *output = {value.scene.body, value.scene.scroll_dips, ui_pinned_messages_scene_scroll_maximum(value.scene),
               40., 1., 0., value.source.revision};
    return true;
}
bool application_pinned_messages_scroll_to(double offset) noexcept {
    return application_pinned_messages_active() &&
        ui_pinned_messages_scene_scroll(&g_app.pinned_messages->scene, static_cast<float>(offset)) && repaint();
}
bool application_pinned_messages_window_message(UINT message, WPARAM, LPARAM lparam) noexcept {
    auto* value = g_app.pinned_messages;
    if (!front()) return false;
    const float scale = 96.f / maximum(g_app.renderer.dpi, 1u);
    const float x = static_cast<short>(LOWORD(lparam)) * scale;
    const float y = static_cast<short>(HIWORD(lparam)) * scale;
    const auto pin_rectangle = ui_scene_header_geometry(g_app.shell_scene).pins;
    const bool header = ui_rect_contains(pin_rectangle, x, y);
    const bool inside = value && application_pinned_messages_active() && ui_pinned_messages_scene_contains(value->scene, x, y);
    if (message == WM_LBUTTONDOWN && header) {
        value = ensure_owner();
        if (!value) return true;
        value->pressed = {UiPinnedMessagesAction::none, scope().channel_id, ~u64{0}};
        value->pressed_scope = scope();
        value->captured = true;
        SetCapture(g_app.window);
        return true;
    }
    if (!value) return false;
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE) { clear_capture(*value); return false; }
    if (message == WM_LBUTTONUP && value->captured) {
        const auto pressed = value->pressed;
        const bool header_press = pressed.message_id == ~u64{0};
        const auto hit = inside ? ui_pinned_messages_scene_hit_test(value->scene, x, y) : UiPinnedMessagesTarget{};
        const bool accepted = header_press ? header && pinned_message_scope_equal(value->pressed_scope, scope()) :
            value->pressed_source_revision == value->controller.revision &&
            ui_pinned_messages_target_equal(pressed, hit);
        clear_capture(*value);
        if (accepted) {
            if (header_press) application_pinned_messages_toggle(); else activate(hit);
        }
        return true;
    }
    if (!inside && !value->scrollbar_dragging) {
        if (message == WM_MOUSEMOVE && application_pinned_messages_active()) {
            bool changed = false;
            ui_pinned_messages_scene_set_hover(&value->scene, -1.f, -1.f, &changed);
            if (changed) application_request_frame();
        }
        return false;
    }
    if (message == WM_MOUSEMOVE && value->scrollbar_dragging) {
        const auto bar = ui_scrollbar_geometry(value->scene.body, value->scene.scroll_dips,
            ui_rect_height(value->scene.body) + ui_pinned_messages_scene_scroll_maximum(value->scene));
        application_pinned_messages_scroll_to(ui_scrollbar_scroll_from_pointer(bar, y, value->thumb_grab));
        return true;
    }
    if (message == WM_MOUSEMOVE) {
        bool changed = false;
        ui_pinned_messages_scene_set_hover(&value->scene, x, y, &changed);
        if (changed) application_request_frame();
        return true;
    }
    if (message == WM_LBUTTONDOWN) {
        const auto bar = ui_scrollbar_geometry(value->scene.body, value->scene.scroll_dips,
            ui_rect_height(value->scene.body) + ui_pinned_messages_scene_scroll_maximum(value->scene));
        value->pressed = ui_pinned_messages_scene_hit_test(value->scene, x, y);
        if (ui_scrollbar_track_hit(bar, x, y)) {
            value->pressed = {};
            value->scrollbar_dragging = ui_scrollbar_thumb_hit(bar, x, y);
            value->thumb_grab = y - bar.thumb.top;
            if (!value->scrollbar_dragging) application_pinned_messages_scroll_to(value->scene.scroll_dips +
                (y < bar.thumb.top ? -1 : 1) * ui_rect_height(value->scene.body));
        } else ui_pinned_messages_scene_focus(&value->scene, value->pressed);
        value->pressed_source_revision = value->controller.revision;
        value->captured = true;
        g_app.context_focus = &value->scene.widgets;
        SetFocus(g_app.window);
        SetCapture(g_app.window);
        application_request_frame();
        return true;
    }
    return message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN ||
        message == WM_RBUTTONUP || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP;
}
bool application_pinned_messages_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    auto* value = g_app.pinned_messages;
    if (!value) return false;
    if (ui_input_consume_key_tail(&value->held_key, {kind, code, repeated})) return true;
    if (!application_pinned_messages_active() || !front()) return false;
    if (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down)
        return (kind == UiInputKind::character || kind == UiInputKind::system_character) &&
            g_app.context_focus == &value->scene.widgets;
    if (shortcut_matches(ShortcutAction::search, shortcut_chord_from_keyboard(code))) { application_pinned_messages_close(); return false; }
    if (code == VK_ESCAPE) { value->held_key = code; application_pinned_messages_close(); repaint(); return true; }
    if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) return false;
    if (g_app.context_focus != &value->scene.widgets) return false;
    if (code == VK_RETURN || code == VK_SPACE) {
        if (!repeated) { value->held_key = code; activate(ui_pinned_messages_scene_focused_target(value->scene)); }
        return true;
    }
    if (code == 'X' && (GetKeyState(VK_SHIFT) & 0x8000)) {
        const auto target = ui_pinned_messages_scene_focused_target(value->scene);
        if (!repeated && target.message_id) { value->held_key = code; application_pinned_messages_action(target.message_id, false, false); }
        return true;
    }
    UiPinnedMessagesFocusMove move{};
    if (code == VK_TAB) move = (GetKeyState(VK_SHIFT) & 0x8000) ? UiPinnedMessagesFocusMove::previous : UiPinnedMessagesFocusMove::next;
    else if (code == VK_DOWN || code == VK_RIGHT) move = UiPinnedMessagesFocusMove::next;
    else if (code == VK_UP || code == VK_LEFT) move = UiPinnedMessagesFocusMove::previous;
    else if (code == VK_HOME) move = UiPinnedMessagesFocusMove::first;
    else if (code == VK_END) move = UiPinnedMessagesFocusMove::last;
    else if (code == VK_PRIOR) move = UiPinnedMessagesFocusMove::page_previous;
    else if (code == VK_NEXT) move = UiPinnedMessagesFocusMove::page_next;
    else return value->scene.overlay;
    ui_pinned_messages_scene_move_focus(&value->scene, move);
    application_request_frame();
    return true;
}
} // namespace maicord::app_detail
