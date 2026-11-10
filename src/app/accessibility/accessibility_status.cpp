// What both accessibility servers run beside the snapshot (accessibility_describe.cpp) and its actions
// (accessibility_actions.cpp): the deferred action queue, text geometry, and what a screen reader is told that no node
// says: a new message in the open conversation, a connection that came or went, a failed send, a finished upload, a
// toast, joining voice, a mention, the result counts of the quick switcher and search. Windows raises those as UI
// Automation notifications (accessibility.cpp), Linux as AT-SPI announcements (platform/atspi).
#include "accessibility_internal.hpp"
#include "maicord/core/time.hpp"
#include "../chat/composer/text_input.hpp"
#include "../account/friends.hpp"
#include "../chat/messages/feedback.hpp"
#include "../shell/window_state.hpp"
#include "maicord/app/voice/socket.hpp"
#include "maicord/app/voice/udp_discovery.hpp"
#include "maicord/app/chat/messages/interaction_rules.hpp"
#if !defined(_WIN32)
#include "maicord/platform/atspi/server.hpp"
#endif

namespace maicord::app_detail {
namespace {

void announce_new_message(const UiAccessTree& tree) noexcept {
    const auto& scene = g_app.message_scene;
    const auto* state = chat_state();
    if (!state || g_app.login.visible || application_friends_active() || !scene.active || !scene.timeline ||
        !scene.timeline->store)
        return;
    const u32 count = message_scene_source_count(scene);
    const MessageRecord* record =
        count ? message_store_resolve(*scene.timeline->store, message_scene_source_handle(scene, count - 1)) : nullptr;
    const bool local = record && (record->author_id == state->self_user_id ||
                                  record->delivery != MessageDeliveryState::authoritative);
    if (!ui_access_message_should_announce(&g_access.messages, state->selected_channel_id, record ? record->id : 0,
                                           local) ||
        !application_window_foreground())
        return;
    const auto& users = g_app.account_ui->user_store;
    const auto* user = user_store_find(users, record->author_id);
    g_name.bytes = 0;
    if (user)
        g_name.add(user_store_name(users, *user)).add(": ");
    // Prefer the drawn (markdown-resolved) content of a materialized row.
    const u32 before = g_name.bytes;
    const auto* presentation = g_app.renderer.message_text;
    for (u32 row = 0; presentation && row < scene.count; ++row) {
        if (scene.rows[row].message_id != record->id)
            continue;
        for (u32 index = 0; index < presentation->command_count && index < presentation->commands.count; ++index) {
            const auto& command = presentation->commands[index];
            if (command.message_id == record->id && command.owner.manager == &scene.widgets &&
                ui_widget_handle_equal(command.owner.handle, scene.rows[row].content))
                g_name.add(command.text).add(" ");
        }
        break;
    }
    if (g_name.bytes == before) {
        const auto found = message_store_find(*scene.timeline->store, record->id);
        if (found.ok())
            g_name.add(message_store_content(*scene.timeline->store, found.handle));
    }
    const u32 list = ui_access_widget_node(tree, {&scene.widgets, scene.root});
    const u64 key = list == kUiAccessNone ? 0 : access_event_key(tree, list);
#if defined(_WIN32)
    uia_host_raise_notification(&g_access.host, key, UiaNotificationKind::item_added, UiaNotificationProcessing::all,
                                g_name.view(), utf8_literal("MaiCord.NewMessage"));
#else
    atspi::server_announce(key, g_name.view(), false);
#endif
}

// `activity` names the kind of announcement for UI Automation clients; AT-SPI has no such field.
void announce(const Utf8View& text, bool assertive,
              [[maybe_unused]] const Utf8View& activity = utf8_literal("MaiCord.Status")) noexcept {
    if (!ui_access_announce_admit(&g_access.announcer, text,
                                  assertive ? UiAccessPoliteness::assertive : UiAccessPoliteness::polite,
                                  clock_monotonic_milliseconds()))
        return;
#if defined(_WIN32)
    uia_host_raise_notification(&g_access.host, 0, UiaNotificationKind::other,
                                assertive ? UiaNotificationProcessing::important_all
                                          : UiaNotificationProcessing::most_recent,
                                text, activity);
#else
    atspi::server_announce(0, text, assertive);
#endif
}

u64 text_hash(const Utf8View& text) noexcept {
    return text.bytes ? hash64_bytes(kHash64Seed, text.data, text.bytes) : 0;
}

// Reconnecting / offline strip above the composer.
void observe_connection(StatusObserver& status, bool primed) noexcept {
    const Utf8View notice = application_composer_status_notice();
    const Utf8View reconnecting = connection_notice_text(ConnectionNoticeState::reconnecting);
    const Utf8View offline = connection_notice_text(ConnectionNoticeState::offline);
    const char* shown = notice.data == reconnecting.data || notice.data == offline.data ? notice.data : nullptr;
    if (primed && shown != status.connection) {
        if (shown == reconnecting.data)
            announce(reconnecting, false);
        else if (shown == offline.data)
            announce(offline, true);
        else if (status.connection)
            announce(utf8_literal("Connected"), false);
    }
    status.connection = shown;
}

// Composer send failures and failed deliveries in the open conversation.
void observe_sends(StatusObserver& status, bool primed) noexcept {
    const MessageComposerPhase phase = g_app.message_composer.phase;
    if (primed && phase == MessageComposerPhase::failed && status.composer_phase != MessageComposerPhase::failed)
        announce(utf8_literal("Message failed to send"), true);
    status.composer_phase = phase;
    const auto& scene = g_app.message_scene;
    const auto* state = chat_state();
    u32 failed = 0;
    if (state && scene.active && scene.timeline && scene.timeline->store) {
        const u32 count = message_scene_source_count(scene);
        for (u32 index = count > 8 ? count - 8 : 0; index < count; ++index) {
            const auto* record = message_store_resolve(*scene.timeline->store, message_scene_source_handle(scene, index));
            failed += record && record->delivery == MessageDeliveryState::failed ? 1u : 0u;
        }
    }
    const u64 channel = state ? state->selected_channel_id : 0;
    if (primed && channel == status.failed_channel && failed > status.failed_count)
        announce(utf8_literal("Message failed to send"), true);
    status.failed_channel = channel;
    status.failed_count = failed;
}

// Attachment uploads finishing or failing.
void observe_uploads(StatusObserver& status, bool primed) noexcept {
    const auto* attachments = g_app.attachments;
    for (u32 index = 0; index < kMessageAttachmentDraftCapacity; ++index) {
        UploadObservation now{};
        if (attachments && attachments->drafts.records[index].occupied) {
            const auto& record = attachments->drafts.records[index];
            now = {record.nonce, record.generation, record.phase, true};
        }
        auto& before = status.uploads[index];
        const bool same = now.occupied && before.occupied && before.nonce == now.nonce &&
                          before.generation == now.generation;
        const bool in_flight = before.phase == MessageAttachmentDraftPhase::uploading ||
                               before.phase == MessageAttachmentDraftPhase::waiting_url;
        if (primed && same && in_flight && now.phase == MessageAttachmentDraftPhase::ready)
            announce(utf8_literal("Upload complete"), false);
        else if (primed && same && now.phase == MessageAttachmentDraftPhase::failed &&
                 before.phase != MessageAttachmentDraftPhase::failed)
            announce(utf8_literal("Upload failed"), true);
        before = now;
    }
}

// Each toast is read once as it appears; failures interrupt.
void observe_toast(StatusObserver& status, bool primed) noexcept {
    const auto& toasts = g_app.toasts;
    const Utf8View text = ui_toast_current_text(toasts);
    if (!text.bytes || toasts.serial == status.toast_serial)
        return;
    status.toast_serial = toasts.serial;
    if (primed)
        announce(text, ui_toast_current_kind(toasts) == UiToastKind::failure,
                 utf8_literal("MaiCord.Toast"));
}

// Joining and leaving voice.
void observe_voice(StatusObserver& status, bool primed) noexcept {
    const auto* session = g_app.account_ui ? &g_app.account_ui->voice_session : nullptr;
    const u64 channel = session && session->phase != VoiceSetupPhase::idle ? session->channel_id : 0;
    const bool connected = channel && session->socket_phase == VoiceSocketPhase::open &&
                           session->udp_phase == VoiceUdpPhase::ready;
    if (primed && connected && (!status.voice_connected || channel != status.voice_channel)) {
        g_name.bytes = 0;
        g_name.add("Voice connected");
        const Utf8View name = channel_name(channel);
        if (name.bytes)
            g_name.add(", ").add(name);
        announce(g_name.view(), false);
    } else if (primed && !channel && status.voice_channel) {
        announce(utf8_literal("Voice disconnected"), false);
    }
    status.voice_channel = channel;
    status.voice_connected = connected;
}

// New mentions anywhere (sampled; the unread counters are authoritative).
void observe_mentions(StatusObserver& status, u64 now_ms) noexcept {
    const auto* state = chat_state();
    if (!state || (status.mentions_primed && now_ms - status.mention_sampled_ms < kMentionSampleMs))
        return;
    status.mention_sampled_ms = now_ms;
    u64 total = 0, newest = 0;
    const ChannelRecord* latest = nullptr;
    for (u32 index = 0; index < state->channel_count; ++index) {
        const auto& channel = state->channels[index];
        if (!channel.occupied || !channel.mention_count)
            continue;
        total += channel.mention_count;
        if (channel.last_message_id >= newest) {
            newest = channel.last_message_id;
            latest = &channel;
        }
    }
    if (status.mentions_primed && total > status.mention_total && latest) {
        g_name.bytes = 0;
        g_name.add("New mention");
        const Utf8View name = chat_state_store_channel_name(*state, *latest);
        if (name.bytes)
            g_name.add(channel_is_private(latest->type) ? utf8_literal(" from ") : utf8_literal(" in #")).add(name);
        announce(g_name.view(), false, utf8_literal("MaiCord.Mention"));
    }
    status.mention_total = total;
    status.mentions_primed = true;
}

// Result counts of the quick switcher and search while they are open.
void observe_results(StatusObserver& status, bool primed) noexcept {
    const auto& renderer = g_app.renderer;
    UiAccessNameBuilder summary{};
    u64 quick = 0, search = 0;
    if (d3d11_renderer_shows<d3d11_surface_quick_switcher>(renderer) &&
        ui_access_quick_switcher_summary(*renderer.quick_switcher_scene, &summary)) {
        quick = text_hash(summary.view());
        if (primed && quick != status.quick_summary)
            announce(summary.view(), false, utf8_literal("MaiCord.Results"));
    }
    if (d3d11_renderer_shows<d3d11_surface_search>(renderer) &&
        ui_access_search_summary(*renderer.search_scene, &summary)) {
        search = text_hash(summary.view());
        if (primed && search != status.search_summary)
            announce(summary.view(), false, utf8_literal("MaiCord.Results"));
    }
    status.quick_summary = quick;
    status.search_summary = search;
}

void observe_status(u64 now_ms) noexcept {
    auto& status = g_access.status;
    if (g_app.login.visible || !g_app.account_ui) {
        status.primed = status.mentions_primed = false;
        return;
    }
    // A new account session starts from its current state, silently.
    if (status.account_instance != g_app.account.instance) {
        reset_value(&status);
        status.account_instance = g_app.account.instance;
    }
    const bool primed = status.primed;
    observe_connection(status, primed);
    observe_sends(status, primed);
    observe_uploads(status, primed);
    observe_toast(status, primed);
    observe_voice(status, primed);
    observe_results(status, primed);
    observe_mentions(status, now_ms);
    status.primed = true;
}

} // namespace
namespace accessibility_detail {

void run_pending() noexcept {
    PendingAction actions[kPendingCapacity];
    const u32 count = g_access.pending_count;
    for (u32 index = 0; index < count; ++index)
        actions[index] = g_access.pending[index];
    g_access.pending_count = 0;
    for (u32 index = 0; index < count; ++index) {
        g_access.dirty = true;
        run_action(actions[index]);
    }
    g_access.dirty = true;
    // The next frame reports the action's result at once.
    ui_access_throttle_mark_urgent(&g_access.throttle);
}

u32 access_text_bounds(void*, u64 key, u32 start, u32 end, UiRect* rectangles, u32 capacity) noexcept {
    const auto& tree = g_access.tree;
    const u32 index = tree.finished && capacity ? ui_access_find(tree, key) : kUiAccessNone;
    if (index == kUiAccessNone || !(tree.nodes[index].state & ui_access_focused))
        return 0;
    return application_composer_text_input_range_bounds(ui_access_value(tree, index), start, end, rectangles) ? 1 : 0;
}

void announce_status(const UiAccessTree& tree, u64 now_ms) noexcept {
    announce_new_message(tree);
    observe_status(now_ms);
}

void forget_status() noexcept {
    g_access.status.primed = g_access.status.mentions_primed = false;
}

} // namespace accessibility_detail

} // namespace maicord::app_detail
