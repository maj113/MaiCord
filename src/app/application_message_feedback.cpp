#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"

#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_message_edit.hpp"
#include "application_message_history.hpp"
#include "application_message_reaction.hpp"
#include "application_state.hpp"

#include "maicord/app/gateway_client.hpp"
#include "maicord/app/message_interaction_rules.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/ui/emoji_catalog.hpp"
#include "maicord/ui/message_rich_layout.hpp"
#include "maicord/ui/message_scene.hpp"

namespace maicord::app_detail {
namespace {
struct ComposerFeedback {
    const void* account_instance;
    ConnectionNotice connection;
    bool connected_once, refresh_pending;
    u64 highlight_message_id, highlight_started_ms;
    u64 slowmode_channel_id, slowmode_until_ms;
    char slowmode_label[kSlowmodeLabelBytes];
    u32 slowmode_label_bytes;
};
ComposerFeedback feedback;

void feedback_bind_account() noexcept {
    if (feedback.account_instance == g_app.account.instance)
        return;
    reset_value(&feedback);
    feedback.account_instance = g_app.account.instance;
}

// Only resolved channel permissions can exempt the local slowmode prediction.
u32 slowmode_seconds(u64 channel_id) noexcept {
    const auto* channel =
        g_app.account_ui ? chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id) : nullptr;
    if (!channel || channel_is_private(channel->type) ||
        (channel->permissions_resolved &&
         (channel->effective_permissions & kPermissionBypassSlowmode)))
        return 0;
    return channel_slowmode_seconds(*channel);
}

u64 slowmode_remaining(u64 channel_id, u64 now_ms) noexcept {
    feedback_bind_account();
    if (channel_id && channel_id == feedback.slowmode_channel_id) {
        const u64 duration_ms = u64{slowmode_seconds(channel_id)} * 1000;
        if (!duration_ms) {
            // This deadline predicts channel slowmode; server rate limits belong
            // to the send controller and REST scheduler and remain authoritative.
            feedback.slowmode_channel_id = feedback.slowmode_until_ms = 0;
        } else if (now_ms < feedback.slowmode_until_ms &&
                   feedback.slowmode_until_ms - now_ms > duration_ms) {
            // A shorter policy caps the remaining wait. Never extend a running
            // prediction; this sum is strictly before its existing deadline.
            feedback.slowmode_until_ms = now_ms + duration_ms;
        }
    }
    return channel_id && channel_id == feedback.slowmode_channel_id && now_ms < feedback.slowmode_until_ms
               ? feedback.slowmode_until_ms - now_ms
               : 0;
}

const MessageRecord* latest_message(u64 channel_id) noexcept {
    const MessageStore& store = g_app.message_view.store;
    for (u32 index = store.count; index-- > 0;) {
        const MessageRecord* record = message_store_resolve(store, message_store_ordered_handle(store, index));
        if (record && record->channel_id == channel_id && record->delivery == MessageDeliveryState::authoritative)
            return record;
    }
    return nullptr;
}

bool react_to_latest(u64 channel_id, Utf8View name) noexcept {
    const MessageRecord* target = latest_message(channel_id);
    if (!target)
        return false;
    const ConstSlice<EmojiCatalogEntry> catalog = unicode_emoji_catalog();
    for (u32 index = 0; index < catalog.count; ++index)
        if (utf8_equal(catalog[index].name(), name))
            return queue_message_reaction_emoji(target->id, 0, catalog[index].glyph(), false);
    const ChatStateStore& state = g_app.account_ui->chat_state;
    for (u32 index = 0; state.emojis && index < state.emoji_count; ++index) {
        const CustomEmojiRecord& emoji = state.emojis[index];
        if (utf8_equal(chat_state_store_emoji_name(emoji), name) && chat_state_store_emoji_usable(state, emoji))
            return queue_message_reaction_emoji(target->id, emoji.id, chat_state_store_emoji_name(emoji),
                                                (emoji.flags & custom_emoji_animated) != 0);
    }
    return false;
}

bool clear_composer_text() noexcept {
    MessageComposer& composer = g_app.message_composer;
    return composer_apply_result(message_composer_replace_range(&composer, 0, composer.bytes, utf8_literal("")),
                                 false);
}

bool slowmode_label_refresh(u64 now_ms) noexcept {
    const u64 remaining = g_app.account_ui ? slowmode_remaining(g_app.account_ui->active_channel_id, now_ms) : 0;
    char label[kSlowmodeLabelBytes];
    const u32 bytes = slowmode_label_format(remaining, {label, kSlowmodeLabelBytes});
    if (bytes == feedback.slowmode_label_bytes && bytes_equal(label, feedback.slowmode_label, bytes))
        return false;
    copy_bytes(feedback.slowmode_label, label, bytes);
    feedback.slowmode_label_bytes = bytes;
    return true;
}

bool highlight_tick(u64 now_ms) noexcept {
    if (!feedback.highlight_message_id)
        return false;
    const float level = message_jump_highlight_level(now_ms - feedback.highlight_started_ms);
    const bool changed =
        message_scene_set_highlight(&g_app.message_scene, level > 0 ? feedback.highlight_message_id : 0, level);
    if (level <= 0)
        feedback.highlight_message_id = 0;
    return changed;
}
} // namespace

bool application_composer_prepare_send(bool* handled) noexcept {
    *handled = false;
    MessageComposer& composer = g_app.message_composer;
    if (!g_app.account_ui || g_app.message_edit.editor_active || !composer.channel_id)
        return true;
    const u64 now_ms = clock_monotonic_milliseconds();
    if (slowmode_remaining(composer.channel_id, now_ms)) {
        // Keep the draft; the strip already shows the countdown.
        *handled = true;
        return true;
    }
    ScratchPages<char[kMessageComposerMaximumBytes]> scratch;
    if (!scratch.data)
        return true;
    char* output = *scratch.data;
    const ComposerTextCommand command =
        composer_text_command_parse(message_composer_text(composer), {output, kMessageComposerMaximumBytes});
    switch (command.kind) {
    case ComposerTextCommandKind::rewrite:
        return message_composer_replace_range(&composer, 0, composer.bytes, {output, command.bytes}) !=
               MessageComposerResult::invalid_input;
    case ComposerTextCommandKind::react:
        if (!react_to_latest(composer.channel_id, command.emoji))
            return true;
        *handled = true;
        return clear_composer_text() && pump_message_history();
    case ComposerTextCommandKind::substitute: {
        const u64 target = application_message_edit_latest_own();
        const auto found = target ? message_store_find(g_app.message_view.store, target) : MessageStoreResult{};
        u32 bytes = 0;
        if (!found.ok() ||
            !composer_text_substitute(message_store_content(g_app.message_view.store, found.handle),
                                      command.pattern, command.replacement, command.global,
                                      {output, kMessageComposerMaximumBytes}, &bytes) ||
            !queue_message_edit_content(target, {output, bytes}))
            return true;
        *handled = true;
        return clear_composer_text() && pump_message_history();
    }
    case ComposerTextCommandKind::none:
        break;
    }
    u32 converted = 0;
    if (message_rich_display().convert_emoticons &&
        composer_emoticons_convert(message_composer_text(composer), {output, kMessageComposerMaximumBytes},
                                   &converted))
        return message_composer_replace_range(&composer, 0, composer.bytes, {output, converted}) !=
               MessageComposerResult::invalid_input;
    return true;
}

bool application_composer_slowmode_active(u64 channel_id) noexcept {
    return slowmode_remaining(channel_id, clock_monotonic_milliseconds()) != 0;
}

void application_composer_note_sent(u64 channel_id) noexcept {
    const u32 seconds = slowmode_seconds(channel_id);
    feedback_bind_account();
    if (!seconds)
        return;
    feedback.slowmode_channel_id = channel_id;
    feedback.slowmode_until_ms = clock_monotonic_milliseconds() + u64{seconds} * 1000;
    slowmode_label_refresh(clock_monotonic_milliseconds());
}

void application_composer_gateway_event(GatewayClientEventKind kind) noexcept {
    feedback_bind_account();
    ConnectionNoticeState state = feedback.connection.raw;
    switch (kind) {
    case GatewayClientEventKind::ready:
    case GatewayClientEventKind::resumed:
        feedback.connected_once = true;
        state = ConnectionNoticeState::connected;
        break;
    case GatewayClientEventKind::reconnecting:
    case GatewayClientEventKind::closed:
    // A failed worker is restarted by the account coordinator with backoff;
    // only rejected credentials end the session, and those leave this view.
    case GatewayClientEventKind::failed:
        state = ConnectionNoticeState::reconnecting;
        break;
    case GatewayClientEventKind::stopped:
        state = ConnectionNoticeState::connected;
        feedback.connected_once = false;
        break;
    default:
        return;
    }
    // The account pump is mid-drain; the next tick re-projects the composer.
    if (connection_notice_observe(&feedback.connection, state, clock_monotonic_milliseconds()))
        feedback.refresh_pending = true;
}

Utf8View application_composer_status_notice() noexcept {
    feedback_bind_account();
    if (feedback.connected_once && feedback.connection.shown != ConnectionNoticeState::connected)
        return connection_notice_text(feedback.connection.shown);
    if (feedback.slowmode_label_bytes && g_app.account_ui &&
        slowmode_remaining(g_app.account_ui->active_channel_id, clock_monotonic_milliseconds()))
        return {feedback.slowmode_label, feedback.slowmode_label_bytes};
    return {};
}

bool application_composer_status_notice_is_error() noexcept {
    return feedback.connected_once && feedback.connection.shown == ConnectionNoticeState::offline &&
           application_account_notice().data == connection_notice_text(ConnectionNoticeState::offline).data;
}

void application_message_highlight_begin(u64 message_id) noexcept {
    feedback_bind_account();
    feedback.highlight_message_id = message_id;
    feedback.highlight_started_ms = message_id ? clock_monotonic_milliseconds() : 0;
    // Reduced motion skips the fades; the hold still marks the target.
    if (message_id && g_app.preferences.reduced_motion)
        feedback.highlight_started_ms -= kMessageJumpHighlightFadeMs;
    if (highlight_tick(clock_monotonic_milliseconds()) && g_app.window)
        application_request_frame();
}

u64 application_message_interaction_next_wake(u64 now_ms) noexcept {
    u64 wake = feedback.highlight_message_id
                   ? message_jump_highlight_next_wake(feedback.highlight_started_ms, now_ms)
                   : 0;
    const u64 connection = feedback.connected_once ? connection_notice_next_wake(feedback.connection) : 0;
    if (connection && (!wake || connection < wake))
        wake = connection;
    if (feedback.slowmode_label_bytes) {
        const u64 remaining = feedback.slowmode_until_ms > now_ms ? feedback.slowmode_until_ms - now_ms : 0;
        const u64 tick = now_ms + (remaining % 1000 ? remaining % 1000 : 1000);
        const u64 next = remaining ? minimum(tick, feedback.slowmode_until_ms) : now_ms;
        if (!wake || next < wake)
            wake = next;
    }
    return wake;
}

void application_message_interaction_tick(u64 now_ms) noexcept {
    feedback_bind_account();
    if (highlight_tick(now_ms) && g_app.window)
        application_request_frame();
    bool composer_changed = connection_notice_tick(&feedback.connection, now_ms) || feedback.refresh_pending;
    feedback.refresh_pending = false;
    if (feedback.slowmode_label_bytes || feedback.slowmode_until_ms) {
        // Apply policy changes even while another channel owns the composer.
        slowmode_remaining(feedback.slowmode_channel_id, now_ms);
        composer_changed = slowmode_label_refresh(now_ms) || composer_changed;
        if (now_ms >= feedback.slowmode_until_ms)
            feedback.slowmode_channel_id = feedback.slowmode_until_ms = 0;
    }
    if (composer_changed && g_app.account_ui)
        refresh_message_composer_ui();
}

} // namespace maicord::app_detail
