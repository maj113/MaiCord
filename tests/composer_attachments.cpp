#include "maicord/app/message_composer_controller.hpp"
#include "maicord/app/message_attachment_storage.hpp"
#include "maicord/ui/message_rich_layout.hpp"
#include "maicord/app/message_history_sync.hpp"

namespace maicord {
void test_composer_attachments(void (*check)(bool, const char*) noexcept) noexcept {
    MessageRecord records[8]{}; u32 order[8]{}, heights[8]{}; u64 fenwick[8]{};
    u8 text[512]{}, draft_text[128]{}, request_text[kMessageComposerMaximumBytes]{};
    char composer_text[128]{};
    MessageDraftRecord draft_records[4]{};
    MessageStore store{}; UiVariableList list{}; MessageTimeline timeline{};
    MessageComposer composer{}; MessageDraftStore drafts{}; MessageComposerController controller{};
    MessageRichRecord rich_records[8]{}; u16 rich_order[8]{}; u8 rich_text[16384]{};
    MessageRichStore rich{};
    const bool initialized = message_store_initialize(&store, {records, 8}, {order, 8}, {text, sizeof(text)}) &&
        ui_variable_list_initialize(&list, {heights, 8}, {fenwick, 8}, 0, 100) &&
        message_timeline_initialize(&timeline, &store, &list, 100, 100, true) &&
        message_rich_store_initialize(&rich, {rich_records, 8}, {rich_order, 8}, {rich_text, sizeof(rich_text)}) &&
        message_timeline_bind_rich_store(&timeline, &rich) &&
        message_composer_initialize(&composer, {composer_text, sizeof(composer_text)}) &&
        message_draft_store_initialize(&drafts, {draft_records, 4}, {draft_text, sizeof(draft_text)}) &&
        message_composer_controller_initialize(&controller, &composer, &drafts, &timeline, {request_text, sizeof(request_text)});
    check(initialized, "attachment send controller fixture initializes");
    if (!initialized) return;
    message_composer_reset(&composer, 7, false);
    char filename[1024]{}, uploaded[4096]{}, description[1024]{};
    for (auto& c : filename) c = 'a';
    for (auto& c : uploaded) c = 'b';
    for (auto& c : description) c = 'c';
    UploadedAttachment attachment{{filename, sizeof(filename)}, {uploaded, sizeof(uploaded)},
        {description, sizeof(description)}, 0, true};
    const u32 bytes = message_attachment_storage_bytes({&attachment, 1});
    check(!message_composer_can_send(composer, true) &&
        message_composer_can_send(composer, true, 2000, true) &&
        !message_composer_can_send(composer, false, 2000, true),
        "attachment-only eligibility preserves channel send permission");
    const u64 file_size = 9876543210ull;
    check(message_composer_controller_set_reply(&controller, 77).ok(),
        "attachment-only reply starts from the actual per-channel draft owner");
    const auto first = message_composer_controller_queue_send(&controller, true, 11, 1725012296789ull, 100, 77, {&attachment, 1}, {&file_size, 1});
    const auto second = message_composer_controller_queue_send(&controller, true, 11, 1725012296790ull, 101, 0, {&attachment, 1});
    check(first.ok() && second.ok() && first.nonce != second.nonce && controller.request_text_used == 2 * bytes + 8 && !composer.bytes,
        "multiple attachment-only requests have distinct nonces and owned storage");
    filename[0] = 'x'; uploaded[0] = 'y'; description[0] = 'z';
    const auto* local = message_rich_store_find(rich, first.nonce);
    check(local && local->attachment_count == 1 && local->attachments[0].size == file_size &&
        message_rich_store_string(rich, local->attachments[0].filename).data[0] == 'a' &&
        message_rich_store_string(rich, local->attachments[0].description).data[0] == 'c' &&
        !local->attachments[0].url.bytes && !local->attachments[0].width && !local->attachments[0].height,
        "pending local attachment owns display metadata without upload URL or source path");
    check(local && message_rich_measure_layout(&rich, local, 280.0f).attachments >= 42.0f,
        "pending attachment-only message reserves visible file card geometry");
    bool restored = false;
    const u32 enqueued = controller.queued;
    check(message_timeline_reset(&timeline) && message_composer_controller_restore_local(&controller, 8, &restored) &&
        !restored && !store.count && message_composer_controller_restore_local(&controller, 7, &restored) && restored &&
        store.count == 2 && controller.queued == enqueued && !controller.inflight_nonce,
        "channel return restores only matching local sends without enqueueing or submitting again");
    local = message_rich_store_find(rich, first.nonce);
    const auto restored_base = message_store_find(store, first.nonce);
    const auto* restored_record = restored_base.ok() ? message_store_resolve(store, restored_base.handle) : nullptr;
    check(local && local->attachments[0].size == file_size && restored_record && restored_record->author_id == 11 &&
        restored_record->timestamp_ms == 1725012296789ull &&
        message_rich_store_string(rich, local->attachments[0].filename).data[0] == 'a',
        "restoration preserves original author time filename and 64-bit source size");
    const u32 created = store.created;
    check(message_composer_controller_restore_local(&controller, 7, &restored) && !restored && store.created == created,
        "resident outbox restoration is idempotent");
    MessageEventPump history_pump{}; history_pump.timeline = &timeline; history_pump.catchup_required = true;
    MessageHistoryCatchup catchup{}; MessageHistoryQuery query{};
    check(message_history_catchup_begin(&catchup, &history_pump, 7, 2) &&
        message_history_catchup_take_query(&catchup, &query) && query.position == MessageHistoryPosition::latest,
        "local-only timeline cannot supply a server history cursor");
    MessageHistoryOlder older{};
    check(message_history_older_demand(&older, &timeline, 7, 2) && older.state == MessageHistoryOlderState::idle &&
        !message_history_older_take_query(&older, &query), "local-only timeline does not imply server history exhaustion");
    GatewayMessageEvent history{}; history.id = 100; history.channel_id = 7; history.author_id = 12;
    history.timestamp_ms = 1725012296700ull; history.content = utf8_literal("history");
    history.fields = gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_author_id |
        gateway_message_field_timestamp | gateway_message_field_content;
    check(message_timeline_apply_history_page(&timeline, {&history, 1}, MessageHistoryDirection::newer,
        MessageContentAvailability::available).ok() && store.count == 1 &&
        message_composer_controller_restore_local(&controller, 7, &restored) && store.count == 3 &&
        message_history_catchup_begin(&catchup, &history_pump, 7, 2) && catchup.cursor_after == 100,
        "server page merges independently then restores outbox with authoritative pagination anchor");
    check(message_history_older_demand(&older, &timeline, 7, 2) && message_history_older_take_query(&older, &query) &&
        query.position == MessageHistoryPosition::before && older.cursor_before == 100,
        "older history becomes available when an authoritative cursor arrives");
    check(message_timeline_reset(&timeline) && message_composer_controller_restore_local(&controller, 7, &restored),
        "outbox can rebuild after another history reset");
    MessageComposerSubmit submit{};
    check(message_composer_controller_prepare_submit(&controller, 102, &submit).ok() &&
        submit.nonce == first.nonce && submit.reply_message_id == 77 && !submit.content.bytes && submit.attachment_count == 1 &&
        submit.attachments[0].filename.data[0] == 'a' && submit.attachments[0].uploaded_filename.data[0] == 'b' &&
        submit.attachments[0].description.data[0] == 'c' && submit.attachments[0].spoiler,
        "queued attachment metadata survives caller mutation and retains reply ownership");
    const auto alias = message_composer_controller_queue_send(&controller, true, 11, 1725012296791ull, 103, 0,
        {submit.attachments, submit.attachment_count});
    check(alias.error == MessageComposerControllerError::invalid_input && controller.request_count == 2,
        "request bank aliases are rejected before compaction invalidates views");
    const auto full = message_composer_controller_queue_send(&controller, true, 11, 1725012296791ull, 103, 0, {&attachment, 1});
    check(full.error == MessageComposerControllerError::text_capacity && controller.request_count == 2 && controller.sends.active_count == 2,
        "attachment bank exhaustion cannot create an unowned local send");
    check(message_composer_controller_mark_submitted(&controller, first.nonce).ok() &&
        message_composer_controller_observe_http(&controller, 400, MessageSendHttpDisposition::terminal_failure, 104).ok(),
        "attachment send failure changes local delivery state");
    local = message_rich_store_find(rich, first.nonce);
    const auto base = message_store_find(store, first.nonce);
    const auto* failed = base.ok() ? message_store_resolve(store, base.handle) : nullptr;
    check(local && failed && failed->delivery == MessageDeliveryState::failed && local->message_revision == failed->revision &&
        local->attachment_count == 1 && !message_rich_store_advance_revision(&rich, first.nonce, failed->revision + 1, failed->revision + 2),
        "failed local attachment stays visible and stale rich revisions cannot be revived");
    check(message_timeline_reset(&timeline) && message_composer_controller_retry(&controller, first.nonce, 104).ok() &&
        !store.count && message_composer_controller_restore_local(&controller, 7, &restored) && restored &&
        message_composer_controller_mark_submitted(&controller, first.nonce).ok() &&
        message_composer_controller_observe_http(&controller, 400, MessageSendHttpDisposition::terminal_failure, 104).ok(),
        "retry works after local row eviction and restoration does not perform the retry");
    check(
        message_composer_controller_discard_failed(&controller, first.nonce).ok() && controller.request_text_wasted == bytes + 8,
        "discard releases text and attachment bytes together");
    const auto third = message_composer_controller_queue_send(&controller, true, 11, 1725012296792ull, 105, 0, {&attachment, 1});
    check(third.ok() && controller.compactions == 1 && controller.request_text_used == 2 * bytes &&
        !controller.request_text_wasted && message_composer_controller_prepare_submit(&controller, 106, &submit).ok() &&
        submit.nonce == second.nonce && submit.attachment_count == 1 && submit.attachments[0].filename.data[0] == 'a',
        "compaction retains complete attachment records and refreshes borrowed submission views");
    check(message_composer_controller_mark_submitted(&controller, second.nonce).ok() &&
        message_composer_controller_observe_http(&controller, 429, MessageSendHttpDisposition::retry_wait, 107, 500).ok() &&
        message_composer_controller_select_channel(&controller, 8).ok() &&
        message_composer_controller_prepare_submit(&controller, 500, &submit).ok() &&
        submit.nonce == second.nonce && submit.channel_id == 7 && submit.attachments[0].uploaded_filename.data[0] == 'b',
        "rate-limited attachment retry remains bound to its original channel and upload reference");
    check(message_composer_controller_mark_submitted(&controller, second.nonce).ok() &&
        message_composer_controller_observe_http(&controller, 200, MessageSendHttpDisposition::success, 510).ok() &&
        message_send_tracker_observe_gateway(&controller.sends, &timeline, second.nonce, 7, 11, 900, 515).ok(),
        "attachment-only message enters normal HTTP and Gateway reconciliation");
    MessageSendCompletion completion{};
    check(message_composer_controller_take_completion(&controller, &completion) && completion.nonce == second.nonce &&
        !message_rich_store_find(rich, second.nonce) &&
        controller.request_count == 1 && message_composer_controller_prepare_submit(&controller, 520, &submit).ok() &&
        submit.nonce == third.nonce && submit.attachments[0].filename.data[0] == 'x',
        "accepted request clears its own attachments without altering later sends");
    check(message_composer_controller_mark_submitted(&controller, third.nonce).ok() &&
        message_composer_controller_observe_http(&controller, 400, MessageSendHttpDisposition::terminal_failure, 530).ok() &&
        message_composer_controller_discard_failed(&controller, third.nonce).ok() && !controller.request_count &&
        !controller.request_text_used && !controller.request_text_wasted,
        "last request discard resets the shared attachment bank");
    check(!rich.count, "accepted or discarded local attachments release rich records");
    GatewayMessageAttachment invalid_local{};
    invalid_local.filename = utf8_literal("broken.txt");
    const auto rejected_local = message_send_tracker_begin(&controller.sends, &timeline, 8, 11,
        {}, 1725012296799ull, 539, true, {&invalid_local, 1});
    check(!rejected_local.ok() && !store.count && !list.count && !rich.count && !controller.sends.active_count,
        "failed local rich insertion cannot leave an orphan send or timeline row");
    bool full_history = true;
    for (u32 i = 0; i < 8; ++i) full_history = message_timeline_insert_local(&timeline,
        {1000 + i, 8, 11, 1725012296800ull + i, 0, utf8_literal("retained"), 0,
            MessageContentAvailability::available, MessageDeliveryState::pending}).ok() && full_history;
    const auto invalid_full = message_send_tracker_begin(&controller.sends, &timeline, 8, 11,
        {}, 1725012296899ull, 540, true, {&invalid_local, 1});
    check(full_history && !invalid_full.ok() && store.count == 8 && list.count == 8 &&
        message_store_find(store, 1000).ok() && !controller.sends.active_count,
        "invalid attachment metadata is rejected before resident history eviction");
    for (u32 i = 0; i < 8; ++i) message_timeline_remove_local(&timeline, 1000 + i);
    bool erased = true;
    for (const auto byte : request_text) erased = erased && byte == 0;
    check(erased, "discard and compaction erase retained attachment strings");
    submit.attachment_count = 10;
    submit.nonce = 123;
    check(message_composer_controller_prepare_submit(&controller, 540, &submit).error == MessageComposerControllerError::not_found &&
        !submit.attachment_count && !submit.nonce && !submit.content.bytes && !submit.channel_id && !submit.reply_message_id,
        "empty submit result clears ownership fields without exposing inactive attachment slots");
    u8 packed[128]{};
    const UploadedAttachment small{utf8_literal("a.txt"), utf8_literal("uploads/a"), {}, 0, false};
    const u32 small_bytes = message_attachment_storage_bytes({&small, 1});
    u32 count = 99;
    check(message_attachment_storage_write({&small, 1}, {packed, sizeof(packed)}) &&
        message_attachment_storage_read({packed, small_bytes}, {submit.attachments, 10}, &count) && count == 1 &&
        !message_attachment_storage_read({packed, small_bytes - 1}, {submit.attachments, 10}, &count) && !count,
        "packed attachments reject truncated records without publishing a count");
    packed[0] = packed[1] = packed[2] = packed[3] = 255;
    check(!message_attachment_storage_read({packed, small_bytes}, {submit.attachments, 10}, &count),
        "packed attachment length overflow is rejected");
    u64 decoded_size = 0;
    check(message_attachment_storage_write({&small, 1}, {packed, sizeof(packed)}, {&file_size, 1}) &&
        message_attachment_storage_bytes({&small, 1}, {&file_size, 1}) == small_bytes + 8 &&
        message_attachment_storage_read({packed, small_bytes + 8}, {submit.attachments, 10}, &count, {&decoded_size, 1}) &&
        count == 1 && decoded_size == file_size && !submit.attachments[0].spoiler &&
        !message_attachment_storage_read({packed, 27}, {submit.attachments, 10}, &count) && !count,
        "optional packed file size survives decoding without changing spoiler or accepting truncation");
    check(message_store_initialize(&store, {records, 2}, {order, 2}, {text, sizeof(text)}) &&
        ui_variable_list_initialize(&list, {heights, 2}, {fenwick, 2}, 0, 100) &&
        message_timeline_initialize(&timeline, &store, &list, 100, 100, true) &&
        message_timeline_bind_rich_store(&timeline, &rich), "bounded outbox cache fixture");
    u64 nonces[3]{};
    for (u32 i = 0; i < 3; ++i) {
        message_composer_insert_utf8(&composer, utf8_literal("restore"));
        const auto pending = message_composer_controller_queue_send(&controller, true, 11,
            1725012297000ull + i, 600 + i);
        nonces[i] = pending.nonce;
        check(pending.ok(), "bounded outbox request queues independently of resident capacity");
    }
    const u32 bounded_created = store.created;
    check(message_composer_controller_restore_local(&controller, 8, &restored) && !restored &&
        store.created == bounded_created && store.count == 2 && !message_store_find(store, nonces[0]).ok(),
        "small cache retains newest outbox suffix without restoration thrashing");
    check(message_timeline_reset(&timeline) && message_timeline_set_scroll(&timeline, 0, false) &&
        message_composer_controller_restore_local(&controller, 8, &restored) && !restored && !store.count &&
        message_timeline_set_scroll(&timeline, 0, true) &&
        message_composer_controller_restore_local(&controller, 8, &restored) && restored && store.count == 2,
        "older history browsing stays untouched until returning to newest messages");
    check(message_send_tracker_observe_gateway(&controller.sends, &timeline, nonces[0], 8, 11, 800, 610).ok() &&
        message_send_tracker_observe_gateway(&controller.sends, &timeline, nonces[2], 8, 11, 802, 611).ok() &&
        message_timeline_reset(&timeline) && message_composer_controller_restore_local(&controller, 8, &restored) &&
        store.count == 1 && message_store_find(store, nonces[1]).ok() &&
        message_composer_controller_mark_submitted(&controller, nonces[2]).ok() &&
        message_composer_controller_observe_http(&controller, 200, MessageSendHttpDisposition::success, 612).ok() &&
        message_composer_controller_take_completion(&controller, &completion) && completion.nonce == nonces[2] &&
        message_composer_controller_restore_local(&controller, 8, &restored) && !restored && store.count == 1,
        "Gateway acknowledgement suppresses resurrection before and after delayed HTTP completion");

    check(message_timeline_reset(&timeline) &&
        message_composer_controller_initialize(&controller, &composer, &drafts, &timeline, {request_text, sizeof(request_text)}),
        "history confirmation fixture resets bounded outbox");
    message_composer_reset(&composer, 8, false);
    const auto recovered = message_composer_controller_queue_send(&controller, true, 11,
        1725012298000ull, 1000, 0, {&small, 1}, {&file_size, 1});
    check(recovered.ok() && message_composer_controller_mark_submitted(&controller, recovered.nonce).ok() &&
        message_composer_controller_observe_http(&controller, 200, MessageSendHttpDisposition::success, 1010).ok(),
        "history recovery starts with HTTP success and a missed Gateway event");
    history.id = 1000; history.channel_id = 8; history.author_id = 11; history.nonce = recovered.nonce;
    bool confirmed = false;
    check(message_composer_controller_observe_history(&controller, {&history, 1}, &confirmed) && !confirmed && controller.request_count == 1,
        "history with absent nonce field cannot confirm a local send");
    history.fields |= gateway_message_field_nonce;
    history.author_id = 12;
    check(message_composer_controller_observe_history(&controller, {&history, 1}, &confirmed) && !confirmed,
        "another author's matching nonce cannot confirm a local send");
    history.author_id = 11; history.channel_id = 7;
    check(message_composer_controller_observe_history(&controller, {&history, 1}, &confirmed) && !confirmed,
        "another channel's matching nonce cannot confirm a local send");
    history.channel_id = 8;
    check(message_timeline_apply_history_page(&timeline, {&history, 1}, MessageHistoryDirection::newer,
        MessageContentAvailability::available).ok() &&
        message_composer_controller_observe_history(&controller, {&history, 1}, &confirmed) && confirmed &&
        message_composer_controller_take_completion(&controller, &completion) && completion.history_observed &&
        !completion.gateway_observed && !completion.gateway_elapsed_ms && completion.http_observed && completion.api_elapsed_ms == 10 &&
        completion.authoritative_id == history.id && completion.kind == MessageSendCompletionKind::accepted &&
        !controller.request_count && !controller.request_text_used && !rich.count &&
        message_composer_controller_restore_local(&controller, 8, &restored) && !restored && store.count == 1,
        "applied history confirms delivery releases attachment payload and never fabricates Gateway latency");
    check(message_composer_controller_observe_history(&controller, {&history, 1}, &confirmed) && !confirmed &&
        !message_composer_controller_take_completion(&controller, &completion) &&
        message_send_tracker_observe_gateway(&controller.sends, &timeline, recovered.nonce, 8, 11, history.id, 1020).ok() &&
        !message_composer_controller_take_completion(&controller, &completion),
        "repeated history and late Gateway acknowledgement cannot complete twice");

    // Exercise receipt ownership both before and after the delivery completion
    // is consumed, including a response that would normally schedule a retry.
    for (u32 drain_first = 0; drain_first < 2; ++drain_first) {
        message_composer_insert_utf8(&composer, utf8_literal("in flight"));
        const auto pending = message_composer_controller_queue_send(&controller, true, 11,
            1725012298100ull + drain_first, 1100 + drain_first);
        message_composer_insert_utf8(&composer, utf8_literal("next"));
        const auto next = message_composer_controller_queue_send(&controller, true, 11,
            1725012298200ull + drain_first, 1200 + drain_first);
        history.nonce = pending.nonce; history.id = 1100 + drain_first;
        check(pending.ok() && next.ok() && message_composer_controller_mark_submitted(&controller, pending.nonce).ok() &&
            message_composer_controller_observe_history(&controller, {&history, 1}, &confirmed) && confirmed,
            "history can confirm an outstanding transport request");
        if (drain_first) check(message_composer_controller_take_completion(&controller, &completion),
            "delivery completion may precede the transport receipt");
        check(controller.inflight_nonce == pending.nonce &&
            message_composer_controller_prepare_submit(&controller, 1300, &submit).error == MessageComposerControllerError::busy &&
            message_composer_controller_observe_http(&controller, 429, MessageSendHttpDisposition::retry_wait, 1301, 1400).ok() &&
            !controller.inflight_nonce, "confirmed send drains its late HTTP response without resubmitting or releasing the next send early");
        if (!drain_first) check(message_composer_controller_take_completion(&controller, &completion),
            "transport receipt may precede consumption of delivery completion");
        check(completion.history_observed && !completion.http_observed && !completion.gateway_observed &&
            message_composer_controller_prepare_submit(&controller, 1302, &submit).ok() && submit.nonce == next.nonce &&
            message_composer_controller_mark_submitted(&controller, next.nonce).ok() &&
            message_composer_controller_observe_http(&controller, 400, MessageSendHttpDisposition::terminal_failure, 1303).ok(),
            "only the next send receives its own HTTP failure");
        history.nonce = next.nonce; history.id = 1200 + drain_first;
        check(message_composer_controller_observe_history(&controller, {&history, 1}, &confirmed) && confirmed &&
            message_composer_controller_take_completion(&controller, &completion) && completion.history_observed &&
            completion.http_status == 400 && !controller.request_count,
            "matching authoritative history recovers a parked failed send");
    }
    for (u32 rate_limited = 0; rate_limited < 2; ++rate_limited) {
        message_composer_insert_utf8(&composer, utf8_literal("slow response"));
        const auto slow = message_composer_controller_queue_send(&controller, true, 11,
            1725012298300ull + rate_limited, 2000);
        const u64 due = 2000 + kMessageSendTrackerTimeoutMs;
        bool expired_changed = false;
        check(slow.ok() && message_composer_controller_mark_submitted(&controller, slow.nonce).ok() &&
            message_composer_controller_tick(&controller, due, &expired_changed) &&
            message_composer_controller_take_completion(&controller, &completion) &&
            completion.kind == MessageSendCompletionKind::expired && controller.inflight_nonce == slow.nonce &&
            message_composer_controller_retry(&controller, slow.nonce, due).error == MessageComposerControllerError::busy &&
            message_composer_controller_observe_http(&controller, rate_limited ? 429 : 200,
                rate_limited ? MessageSendHttpDisposition::retry_wait : MessageSendHttpDisposition::success,
                due + 1, rate_limited ? due + 100 : 0).ok() &&
            message_composer_controller_next_wake_at(controller) > due,
            "late success or rate limit resumes a parked timeout with a bounded deadline and owned receipt");
        const auto resident = message_store_find(store, slow.nonce);
        const auto* record = resident.ok() ? message_store_resolve(store, resident.handle) : nullptr;
        check(record && record->delivery == MessageDeliveryState::pending,
            "late nonterminal response clears the local timeout failure display");
        history.nonce = slow.nonce; history.id = 2000 + rate_limited;
        check(message_composer_controller_observe_history(&controller, {&history, 1}, &confirmed) && confirmed &&
            message_composer_controller_take_completion(&controller, &completion) && !controller.request_count,
            "history confirmation ends the recovered timeout without requiring a resend");
    }
    message_composer_insert_utf8(&composer, utf8_literal("queued expiry"));
    const auto expired_send = message_composer_controller_queue_send(&controller, true, 11, 1725012298400ull, 3000);
    bool expired_changed = false;
    history.nonce = expired_send.nonce; history.id = 3000;
    check(expired_send.ok() && message_composer_controller_mark_submitted(&controller, expired_send.nonce).ok() &&
        message_composer_controller_tick(&controller, 3000 + kMessageSendTrackerTimeoutMs, &expired_changed) &&
        message_composer_controller_observe_history(&controller, {&history, 1}, &confirmed) && confirmed &&
        message_composer_controller_take_completion(&controller, &completion) && completion.kind == MessageSendCompletionKind::expired &&
        message_composer_controller_observe_http(&controller, 200, MessageSendHttpDisposition::success, 34000).ok() &&
        message_composer_controller_take_completion(&controller, &completion) && completion.history_observed &&
        completion.kind == MessageSendCompletionKind::accepted && !controller.request_count && !controller.inflight_nonce,
        "queued expiry cannot overwrite history confirmation before the late transport receipt");
    static char long_text[4096], letters[3000];
    for (auto& c : letters) c = 'a';
    MessageComposer long_composer{};
    bool filled = message_composer_initialize(&long_composer, {long_text, sizeof(long_text)});
    message_composer_reset(&long_composer, 7, false);
    filled = filled && message_composer_insert_utf8(&long_composer, {letters, sizeof(letters)}) == MessageComposerResult::changed;
    check(filled && message_composer_codepoints(long_composer) == 3000 &&
        message_composer_maximum_codepoints(0) == 2000 && message_composer_maximum_codepoints(1) == 2000 &&
        message_composer_maximum_codepoints(3) == 2000 && message_composer_maximum_codepoints(2) == 4000 &&
        !message_composer_can_send(long_composer, true, message_composer_maximum_codepoints(3)) &&
        message_composer_can_send(long_composer, true, message_composer_maximum_codepoints(2)),
        "only full Nitro sends messages past 2000 characters");
}
}
