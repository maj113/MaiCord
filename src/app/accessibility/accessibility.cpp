#include "accessibility_internal.hpp"
#include "accessibility.hpp"
#include "maicord/core/time.hpp"
#include "../chat/composer/text_input.hpp"
#include "../account/friends.hpp"
#include "../chat/messages/feedback.hpp"
#include "../shell/window_state.hpp"
#include "maicord/app/voice/socket.hpp"
#include "maicord/app/voice/udp_discovery.hpp"
#include "maicord/app/chat/messages/interaction_rules.hpp"

namespace maicord::app_detail {
namespace {

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

// Text pattern geometry for the edit that owns native text input.
u32 access_text_bounds(void*, u64 key, u32 start, u32 end, UiRect* rectangles, u32 capacity) noexcept {
    const auto& tree = g_access.tree;
    const u32 index = tree.finished && capacity ? ui_access_find(tree, key) : kUiAccessNone;
    if (index == kUiAccessNone || !(tree.nodes[index].state & ui_access_focused))
        return 0;
    return application_composer_text_input_range_bounds(ui_access_value(tree, index), start, end, rectangles) ? 1 : 0;
}

u64 event_key(const UiAccessTree& tree, u32 node) noexcept {
    return node == 0 || node >= tree.count ? 0 : tree.nodes[node].key;
}

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
    uia_host_raise_notification(&g_access.host, list == kUiAccessNone ? 0 : event_key(tree, list),
                                UiaNotificationKind::item_added, UiaNotificationProcessing::all, g_name.view(),
                                utf8_literal("MaiCord.NewMessage"));
}

void announce(const Utf8View& text, bool assertive,
              const Utf8View& activity = utf8_literal("MaiCord.Status")) noexcept {
    if (!ui_access_announce_admit(&g_access.announcer, text,
                                  assertive ? UiAccessPoliteness::assertive : UiAccessPoliteness::polite,
                                  clock_monotonic_milliseconds()))
        return;
    uia_host_raise_notification(&g_access.host, 0, UiaNotificationKind::other,
                                assertive ? UiaNotificationProcessing::important_all
                                          : UiaNotificationProcessing::most_recent,
                                text, activity);
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
    if (renderer.quick_switcher_scene && d3d11_renderer_quick_switcher_visible(renderer) &&
        ui_access_quick_switcher_summary(*renderer.quick_switcher_scene, &summary)) {
        quick = text_hash(summary.view());
        if (primed && quick != status.quick_summary)
            announce(summary.view(), false, utf8_literal("MaiCord.Results"));
    }
    if (renderer.search_scene && d3d11_renderer_search_visible(renderer) &&
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

void publish_events() noexcept {
    const UiAccessTree* tree = access_tree(nullptr);
    if (!tree)
        return;
    UiAccessEventPlan plan{};
    if (!ui_access_plan_events(&g_access.events, *tree, &plan))
        return;
    if (plan.structure_root)
        uia_host_raise_structure(&g_access.host, 0);
    else
        for (u32 index = 0; index < plan.structure_count; ++index)
            uia_host_raise_structure(&g_access.host, event_key(*tree, plan.structures[index]));
    for (u32 index = 0; index < plan.property_count; ++index) {
        const auto& change = plan.properties[index];
        const u64 key = event_key(*tree, change.node);
        uia_host_raise_property(&g_access.host, key, change);
        if (change.property == UiAccessProperty::name && (tree->nodes[change.node].state & ui_access_live))
            uia_host_raise_live_region(&g_access.host, key);
    }
    if (plan.focus_changed && application_window_keyboard_focus())
        uia_host_raise_focus(&g_access.host, event_key(*tree, plan.focus_node));
    announce_new_message(*tree);
    observe_status(clock_monotonic_milliseconds());
}

bool sample_listening(u64 now_ms) noexcept {
    if (!g_access.listen_sampled_ms || now_ms - g_access.listen_sampled_ms >= kListenSampleMs) {
        g_access.listening = uia_host_listening(g_access.host);
        g_access.listen_sampled_ms = now_ms ? now_ms : 1;
    }
    return g_access.listening;
}

void arm_rebuild_timer(u64 now_ms) noexcept {
    const u64 delay = ui_access_throttle_delay(g_access.throttle, now_ms);
    if (win32_hwnd(g_app.window) && SetTimer(win32_hwnd(g_app.window), kRebuildTimer, static_cast<UINT>(delay ? delay : 1), nullptr))
        g_access.timer = true;
}

// Input that can move focus or change structure makes the next frame urgent.
bool urgent_message(UINT message) noexcept {
    switch (message) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_UNICHAR:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ACTIVATE:
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION:
        return true;
    default:
        return false;
    }
}

} // namespace

bool application_accessibility_window_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) noexcept {
    if (message == kApplicationAccessibilityMessage) {
        g_access.posted = false;
        run_pending();
        *result = 0;
        return true;
    }
    if (g_access.initialized && urgent_message(message))
        ui_access_throttle_mark_urgent(&g_access.throttle);
    if (message == WM_TIMER && wparam == kRebuildTimer) {
        KillTimer(win32_hwnd(g_app.window), kRebuildTimer);
        g_access.timer = false;
        if (g_access.initialized && !g_access.shut_down && g_access.throttle.deferred) {
            const u64 now = clock_monotonic_milliseconds();
            const auto decision = ui_access_throttle_frame(&g_access.throttle, now, sample_listening(now));
            if (decision == UiAccessThrottleDecision::build)
                publish_events();
            else if (decision == UiAccessThrottleDecision::defer)
                arm_rebuild_timer(now);
        }
        *result = 0;
        return true;
    }
    if (message == WM_DESTROY) {
        application_accessibility_shutdown();
        return false;
    }
    // Provider calls marshaled from UI Automation threads onto this thread.
    if (g_access.initialized && g_access.host.call_message && message == g_access.host.call_message)
        return uia_host_window_message(&g_access.host, message, wparam, lparam, result);
    if (message != WM_GETOBJECT || static_cast<long>(lparam) != -25 /* UiaRootObjectId */ || g_access.shut_down ||
        !g_app.window)
        return false;
    if (!g_access.initialized) {
        const UiaHostCallbacks callbacks{nullptr, access_tree, access_act, access_scale, access_text_bounds};
        if (!uia_host_initialize(&g_access.host, win32_hwnd(g_app.window), callbacks))
            return false;
        g_access.initialized = true;
    }
    if (!ensure_storage())
        return false;
    g_access.dirty = true;
    return uia_host_window_message(&g_access.host, message, wparam, lparam, result);
}

void application_accessibility_frame_presented() noexcept {
    // A store and a flag test until a UI Automation client first asks.
    g_access.dirty = true;
    if (!g_access.initialized || g_access.shut_down)
        return;
    const u64 now = clock_monotonic_milliseconds();
    switch (ui_access_throttle_frame(&g_access.throttle, now, sample_listening(now))) {
    case UiAccessThrottleDecision::build:
        publish_events();
        return;
    case UiAccessThrottleDecision::defer:
        // Animation frames coalesce; the timer publishes the trailing state.
        if (!g_access.timer)
            arm_rebuild_timer(now);
        return;
    default:
        // Nobody listens: status observations restart silently later.
        g_access.status.primed = g_access.status.mentions_primed = false;
        return;
    }
}

void application_accessibility_shutdown() noexcept {
    if (g_access.shut_down)
        return;
    g_access.shut_down = true;
    if (g_access.timer && g_app.window)
        KillTimer(win32_hwnd(g_app.window), kRebuildTimer);
    g_access.timer = false;
    if (g_access.initialized)
        uia_host_shutdown(&g_access.host);
}

} // namespace maicord::app_detail
