#include "application_channel_snapshots.hpp"

#include "application_channel_authority.hpp"
#include "application_state.hpp"

#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {

MessageSnapshotCache g_snapshots;
ChannelPrefetch g_prefetch;
HANDLE g_low_memory;
// A dispatch for the still-loaded channel arrived after selection moved away;
// the live window missed it, so it must not be retained.
bool g_active_missed;

MessageSnapshotCache& snapshots() noexcept {
    if (!g_snapshots.budget)
        message_snapshot_cache_initialize(&g_snapshots, kMessageSnapshotCacheBudgetBytes);
    return g_snapshots;
}

bool memory_low() noexcept {
    if (!g_low_memory)
        g_low_memory = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    BOOL low = FALSE;
    return g_low_memory && QueryMemoryResourceNotification(g_low_memory, &low) && low;
}

bool keep_readable(void*, u64 channel_id) noexcept {
    return application_channel_readable(g_app, channel_id);
}

} // namespace

void application_channel_snapshot_capture(u64 channel_id) noexcept {
    const bool missed = g_active_missed;
    g_active_missed = false;
    if (!channel_id || !g_app.account_ui)
        return;
    MessageSnapshotCache& cache = snapshots();
    if (memory_low()) {
        message_snapshot_cache_release(&cache);
        return;
    }
    message_snapshot_cache_prune(&cache, keep_readable, nullptr);
    const MessageHistoryCatchup& catchup = g_app.message_catchup;
    const MessageHistoryOlder& older = g_app.message_older;
    const MessageTimeline& timeline = g_app.message_view.timeline;
    // The active channel never has an entry (restore consumes it), so an
    // incoherent window is simply not retained.
    if (missed || catchup.channel_id != channel_id || !message_history_browsing_ready(catchup.state) ||
        g_app.message_pump.resync_required || g_app.pending_message_jump_message_id ||
        !application_channel_readable(g_app, channel_id))
        return;
    const bool exhausted = older.channel_id == channel_id && older.state == MessageHistoryOlderState::exhausted;
    if (!message_snapshot_cache_capture(&cache, timeline,
                                        {channel_id, exhausted ? older.cursor_before : 0, exhausted,
                                         !timeline.newer_refetch_required, false}))
        log_u64(LogLevel::info, "snapshot", "capture_skipped", channel_id);
}

bool application_channel_snapshot_restore(u64 channel_id) noexcept {
    if (!channel_id || !g_app.account_ui)
        return false;
    MessageSnapshotCache& cache = snapshots();
    if (!message_snapshot_cache_find(cache, channel_id))
        return false;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    if (!channel || !application_channel_readable(g_app, channel_id)) {
        message_snapshot_cache_drop(&cache, channel_id);
        return false;
    }
    const MessageSnapshotRestoreResult restored = message_snapshot_cache_restore(
        &cache, channel_id, channel->last_message_id, kMessageHistoryRequestLimit, &g_app.message_view.timeline);
    if (restored.status != MessageSnapshotRestoreStatus::restored) {
        log_u64(LogLevel::info, "snapshot", restored.status == MessageSnapshotRestoreStatus::gap
                                                ? "restore_gap" : "restore_failed", channel_id);
        return false;
    }
    if (restored.older_exhausted) {
        g_app.message_older = MessageHistoryOlder{};
        g_app.message_older.channel_id = channel_id;
        g_app.message_older.cursor_before = restored.older_cursor;
        g_app.message_older.state = MessageHistoryOlderState::exhausted;
    }
    return true;
}

void application_channel_snapshots_reset() noexcept {
    message_snapshot_cache_release(&g_snapshots);
    g_prefetch = ChannelPrefetch{};
    g_active_missed = false;
}

void application_channel_snapshots_observe(void*, const GatewayClientEvent& event, ConstSlice<u8> payload,
                                           MessageContentAvailability availability) noexcept {
    AccountUiRuntime* ui = g_app.account_ui;
    if (!ui)
        return;
    u64 channel_id = event.message.channel_id;
    if (event.dispatch_kind == GatewayClientDispatchKind::message_reaction_add ||
        event.dispatch_kind == GatewayClientDispatchKind::message_reaction_remove ||
        event.dispatch_kind == GatewayClientDispatchKind::message_reaction_remove_all ||
        event.dispatch_kind == GatewayClientDispatchKind::message_reaction_remove_emoji) {
        GatewayMessageReactionEvent reaction{};
        channel_id = gateway_client_event_as_message_reaction(event, payload, &reaction) ? reaction.channel_id : 0;
    }
    if (channel_id && channel_id == ui->active_channel_id)
        g_active_missed = true;
    message_snapshot_cache_observe(&g_snapshots, event, payload, g_app.message_pump.bulk_id_scratch,
                                   ui->chat_state.self_user_id, availability);
}

void application_channel_prefetch_hover(u64 channel_id) noexcept {
    channel_prefetch_hover(&g_prefetch, channel_id, clock_monotonic_milliseconds());
}

u64 application_channel_prefetch_next_wake() noexcept {
    return g_app.account_ui ? channel_prefetch_next_wake(g_prefetch) : 0;
}

u64 application_channel_prefetch_take(u64 now_ms) noexcept {
    const u64 channel_id = channel_prefetch_due(g_prefetch, now_ms);
    AccountUiRuntime* ui = g_app.account_ui;
    if (!channel_id || !ui)
        return 0;
    if (channel_id == ui->active_channel_id || channel_id == ui->chat_state.selected_channel_id ||
        message_snapshot_cache_find(snapshots(), channel_id) || g_app.message_pump.resync_required ||
        !application_channel_readable(g_app, channel_id)) {
        channel_prefetch_skip(&g_prefetch);
        return 0;
    }
    return channel_id;
}

void application_channel_prefetch_submitted(u64 channel_id, u64 request_id, u64 now_ms) noexcept {
    channel_prefetch_submitted(&g_prefetch, channel_id, request_id, now_ms);
}

void application_channel_prefetch_skip() noexcept {
    channel_prefetch_skip(&g_prefetch);
}

bool application_channel_prefetch_adopt(u64 request_id) noexcept {
    AccountUiRuntime* ui = g_app.account_ui;
    MessageHistoryCatchup& catchup = g_app.message_catchup;
    MessageHistoryQuery query{};
    if (!ui || !ui->active_channel_id || catchup.channel_id != ui->active_channel_id ||
        catchup.state != MessageHistoryCatchupState::request_required || catchup.cursor_after != 0 ||
        catchup.request_limit != kMessageHistoryRequestLimit ||
        !channel_prefetch_adopt(&g_prefetch, request_id, ui->active_channel_id))
        return false;
    return message_history_catchup_take_query(&catchup, &query);
}

bool application_channel_prefetch_response(const RestClientResponse& response, Utf8View body) noexcept {
    const u64 channel_id = g_prefetch.inflight_channel_id;
    const u64 now_ms = clock_monotonic_milliseconds();
    if (!channel_prefetch_complete(&g_prefetch, response.request_id, now_ms))
        return true;
    AccountUiRuntime* ui = g_app.account_ui;
    if (!ui || !channel_id || channel_id == ui->active_channel_id || !response.transport_ok() ||
        response.status_code != 200 || !body.data || !body.bytes || body.bytes != response.body_bytes ||
        message_snapshot_cache_find(snapshots(), channel_id) || !application_channel_readable(g_app, channel_id))
        return true;
    MessageHistoryDecodeScratch scratch{};
    if (!application_message_decode_scratch(*ui, &scratch, body))
        return true;
    const MessageHistoryDecodeResult decoded = decode_message_history_page(
        body, scratch.page_structurals, scratch.message_structurals, scratch.maximum_depth, scratch.content,
        scratch.messages, scratch.mention_users, scratch.rich);
    if (!decoded.ok() || !decoded.page.count || decoded.page.channel_id != channel_id)
        return true;
    const ConstSlice<GatewayMessageEvent> messages{scratch.messages.data, decoded.page.count};
    if (!message_history_observe_page(&ui->user_store, &ui->guild_member_store, &ui->chat_state, messages))
        return true;
    MessagePollPageInput inputs[kMessageHistoryRequestLimit]{};
    MessagePollResultTicket tickets[kMessageHistoryRequestLimit]{};
    u32 polls = 0;
    for (u32 index = 0; index < messages.count && polls < kMessageHistoryRequestLimit; ++index) {
        const GatewayMessageEvent& message = messages[index];
        if (!(message.fields & gateway_message_field_poll))
            continue;
        const auto* existing = message_poll_store_find(ui->message_poll_store, message.id);
        if (existing && existing->last_gateway_serial > ui->message_history_poll_serial)
            continue;
        tickets[polls] =
            message_poll_store_result_ticket(ui->message_poll_store, message.id, ui->message_history_poll_serial);
        inputs[polls] = {message.id, message.channel_id, &message.poll, 0, &tickets[polls]};
        ++polls;
    }
    if (polls && !message_poll_store_apply_page(&ui->message_poll_store, {inputs, polls}).ok())
        return true;
    if (!message_snapshot_cache_capture_page(&g_snapshots, channel_id, messages, &ui->chat_state,
                                             MessageContentAvailability::unknown,
                                             g_app.message_view.timeline.default_item_units,
                                             decoded.page.count < kMessageHistoryRequestLimit))
        log_u64(LogLevel::info, "snapshot", "prefetch_capture_skipped", channel_id);
    return true;
}

} // namespace maicord::app_detail
