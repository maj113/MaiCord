#include "maicord/app/message_composer_controller.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
void test_reply_draft_authority(void (*check)(bool, const char*) noexcept) noexcept {
    MessageRecord records[8]{}; u32 order[8]{}, heights[8]{}; u64 fenwick[8]{};
    u8 text[512]{}, draft_text[128]{}, request_text[kMessageComposerMaximumBytes]{};
    char composer_text[128]{}, source_text[128]{}; MessageDraftRecord draft_records[4]{};
    MessageStore store{}; UiVariableList list{}; MessageTimeline timeline{};
    MessageComposer composer{}, source{}; MessageDraftStore drafts{}; MessageComposerController controller{};
    const bool initialized = message_store_initialize(&store, {records, 8}, {order, 8}, {text, sizeof(text)}) &&
        ui_variable_list_initialize(&list, {heights, 8}, {fenwick, 8}, 0, 100) &&
        message_timeline_initialize(&timeline, &store, &list, 100, 100, true) &&
        message_composer_initialize(&composer, {composer_text, sizeof(composer_text)}) &&
        message_composer_initialize(&source, {source_text, sizeof(source_text)}) &&
        message_draft_store_initialize(&drafts, {draft_records, 4}, {draft_text, sizeof(draft_text)}) &&
        message_composer_controller_initialize(&controller, &composer, &drafts, &timeline, {request_text, sizeof(request_text)}) &&
        message_composer_controller_select_channel(&controller, 7).ok();
    check(initialized, "reply draft authority fixture initializes");
    if (!initialized) return;
    MessageComposerReplySnapshot captured{}, restored{}; MessageDraftSnapshot draft{};
    check(message_composer_controller_set_reply(&controller, 70, false).ok() &&
        message_composer_controller_reply_snapshot(controller, &captured) &&
        message_composer_controller_select_channel(&controller, 8).ok() && !controller.reply_message_id &&
        message_draft_store_snapshot(drafts, 7, &draft) && !draft.text.bytes && draft.reply_message_id == 70 &&
        !draft.mention_reply_author && message_composer_controller_select_channel(&controller, 7).ok() &&
        message_composer_controller_reply_snapshot(controller, &restored) && restored.message_id == 70 &&
        !restored.mention_reply_author && restored.revision != captured.revision,
        "empty reply and disabled mention survive channel navigation with retired producer scope");
    message_composer_insert_utf8(&composer, utf8_literal("keep"));
    message_composer_move(&composer, MessageComposerMove::previous, true);
    check(message_composer_controller_select_channel(&controller, 8).ok() &&
        message_composer_controller_set_reply(&controller, 80).ok() &&
        message_composer_controller_clear_reply(&controller, 7, 70).ok() && controller.reply_message_id == 80 &&
        message_composer_controller_select_channel(&controller, 7).ok() && !controller.reply_message_id &&
        composer.bytes == 4 && composer.anchor == 4 && composer.caret == 3 &&
        message_draft_store_snapshot(drafts, 7, &draft) && !draft.reply_message_id,
        "authoritative deletion clears inactive channel reply while preserving selection text and another reply");
    check(message_composer_controller_set_reply(&controller, 71, false).ok() &&
        message_composer_controller_reply_snapshot(controller, &captured) &&
        message_composer_controller_set_reply(&controller, 0).ok() &&
        message_composer_controller_set_reply(&controller, 71, false).ok(), "same reply reselect creates fresh authority");
    message_composer_restore(&source, 7, utf8_literal("separate"), 8, 8);
    const auto stale = message_composer_controller_queue_send(&controller, true, 11, 1725012296789ull, 10,
        71, {}, {}, &source, {}, false, &captured);
    const auto bypass = message_composer_controller_queue_send(&controller, true, 11, 1725012296789ull, 10);
    check(!stale.ok() && !bypass.ok() && !controller.request_count && source.bytes == 8 && composer.bytes == 4 &&
        controller.reply_message_id == 71 && !controller.mention_reply_author,
        "stale separate producer and ordinary active send cannot bypass authoritative reply choice");
    message_composer_controller_reply_snapshot(controller, &captured);
    const auto denied = message_composer_controller_queue_send(&controller, false, 11, 1725012296789ull, 10,
        71, {}, {}, nullptr, {}, false, &captured);
    const u32 capacity = controller.request_text_capacity;
    controller.request_text_capacity = 1;
    const auto full = message_composer_controller_queue_send(&controller, true, 11, 1725012296789ull, 10,
        71, {}, {}, nullptr, {}, false, &captured);
    controller.request_text_capacity = capacity;
    check(!denied.ok() && full.error == MessageComposerControllerError::text_capacity && composer.bytes == 4 &&
        controller.reply_message_id == 71 && controller.reply_revision == captured.revision,
        "send permission and request capacity failures retain text target ping and producer authority");
    const auto separate = message_composer_controller_queue_send(&controller, true, 11, 1725012296789ull, 11,
        71, {}, {}, &source, {}, false, &captured);
    MessageComposerSubmit submit{};
    check(separate.ok() && !source.bytes && composer.bytes == 4 && !controller.reply_message_id &&
        message_draft_store_snapshot(drafts, 7, &draft) && draft.text.bytes == 4 && !draft.reply_message_id &&
        message_composer_controller_prepare_submit(&controller, 12, &submit).ok() && submit.reply_message_id == 71 &&
        !submit.mention_reply_author && submit.content.bytes == 8,
        "separate producer consumes matching reply but preserves active text and immutable disabled-ping request");
    check(message_composer_controller_set_reply(&controller, 72).ok() &&
        message_composer_controller_mark_submitted(&controller, separate.nonce).ok() &&
        message_composer_controller_observe_http(&controller, 400, MessageSendHttpDisposition::terminal_failure, 13).ok() &&
        controller.reply_message_id == 72 && controller.mention_reply_author && composer.bytes == 4 &&
        message_composer_controller_retry(&controller, separate.nonce, 14).ok() &&
        message_composer_controller_prepare_submit(&controller, 15, &submit).ok() && submit.reply_message_id == 71 &&
        !submit.mention_reply_author,
        "late request failure and retry preserve newer composer choice and original queued reply policy");
    const auto removed = message_composer_controller_clear_reply(&controller, 7, 71);
    check(removed.ok() && removed.changed && controller.reply_message_id == 72 && composer.bytes == 4 &&
        message_composer_controller_mark_submitted(&controller, separate.nonce).error == MessageComposerControllerError::reply_unavailable &&
        message_composer_controller_prepare_submit(&controller, 16, &submit).error == MessageComposerControllerError::reply_unavailable &&
        !submit.reply_message_id && !submit.mention_reply_author &&
        message_composer_controller_retry(&controller, separate.nonce, 17).error == MessageComposerControllerError::reply_unavailable &&
        message_composer_controller_discard_failed(&controller, separate.nonce).ok(),
        "deleted source blocks admitted unsent reply and manual retry without changing newer reply or converting payload");
    message_composer_controller_reply_snapshot(controller, &captured);
    const auto active = message_composer_controller_queue_send(&controller, true, 11, 1725012296790ull, 18,
        72, {}, {}, nullptr, {}, true, &captured);
    check(active.ok() && !composer.bytes && !controller.reply_message_id && controller.mention_reply_author &&
        message_draft_store_find(drafts, 7).error == MessageDraftStoreError::not_found &&
        message_composer_controller_set_reply(&controller, 73, false).ok() &&
        message_composer_controller_prepare_submit(&controller, 19, &submit).ok() && submit.reply_message_id == 72 &&
        submit.mention_reply_author && controller.reply_message_id == 73 && !controller.mention_reply_author,
        "successful active queue clears only its draft while newer empty reply cannot alter queued ping policy");
    check(message_composer_controller_mark_submitted(&controller, active.nonce).ok() &&
        message_composer_controller_clear_reply(&controller, 7, 72).ok() && controller.inflight_nonce == active.nonce &&
        message_composer_controller_observe_http(&controller, 429, MessageSendHttpDisposition::retry_wait, 20, 25).ok() &&
        message_composer_controller_prepare_submit(&controller, 25, &submit).error == MessageComposerControllerError::reply_unavailable &&
        message_composer_controller_retry(&controller, active.nonce, 26).error == MessageComposerControllerError::reply_unavailable &&
        controller.reply_message_id == 73 && message_composer_controller_discard_failed(&controller, active.nonce).ok(),
        "deletion during inflight transport preserves receipt ownership but blocks subsequent automatic and manual retry");
    message_composer_restore(&source, 8, utf8_literal("ordinary"), 8, 8);
    const auto ordinary = message_composer_controller_queue_send(&controller, true, 11, 1725012296791ull, 27,
        0, {}, {}, &source);
    check(ordinary.ok() && controller.reply_message_id == 73 && !controller.mention_reply_author &&
        message_composer_controller_prepare_submit(&controller, 28, &submit).ok() && submit.channel_id == 8 &&
        !submit.reply_message_id && !submit.mention_reply_author &&
        message_composer_controller_cancel_unsent(&controller, ordinary.nonce).ok(),
        "ordinary independent producer cannot borrow or consume active channel reply and always disables reply-author policy");
    message_composer_insert_utf8(&composer, utf8_literal("again"));
    message_composer_controller_reply_snapshot(controller, &captured);
    const auto admitted = message_composer_controller_queue_send(&controller, true, 11, 1725012296792ull, 29,
        73, {}, {}, nullptr, {}, false, &captured);
    check(admitted.ok() && message_composer_controller_prepare_submit(&controller, 30, &submit).ok() &&
        message_composer_controller_mark_submitted(&controller, admitted.nonce).ok() &&
        message_composer_controller_set_reply(&controller, 74).ok() &&
        message_composer_controller_clear_reply(&controller, 7, 73).ok() &&
        message_composer_controller_observe_http(&controller, 200, MessageSendHttpDisposition::success, 31).ok() &&
        message_send_tracker_observe_gateway(&controller.sends, &timeline, admitted.nonce, 7, 11, 900, 32).ok(),
        "source deletion does not prevent successful response from already admitted transport");
    MessageSendCompletion completion{};
    check(message_composer_controller_take_completion(&controller, &completion) && completion.nonce == admitted.nonce &&
        completion.kind == MessageSendCompletionKind::accepted && !controller.request_count && controller.reply_message_id == 74,
        "accepted deleted-source request completion preserves newer reply choice");
    check(message_composer_controller_select_channel(&controller, 9).ok() &&
        message_composer_controller_reply_snapshot(controller, &captured), "empty third channel acquires ordinary authority");
    const u32 draft_capacity = drafts.capacity;
    drafts.capacity = 2;
    check(message_composer_controller_set_reply(&controller, 90, false).error == MessageComposerControllerError::draft &&
        !controller.reply_message_id && controller.mention_reply_author && controller.reply_revision == captured.revision &&
        !composer.bytes, "reply acquisition capacity failure cannot partially change active target policy or revision");
    drafts.capacity = draft_capacity;
    MessageComposerController invalid{};
    check(!message_composer_controller_reply_snapshot(invalid, &captured) && !captured.channel_id && !captured.message_id &&
        !captured.revision, "failed reply capture clears stale producer authority");
}
// Plugin sends: explicit reply and silence, active draft and its reply untouched.
void test_detached_text_send(void (*check)(bool, const char*) noexcept) noexcept {
    MessageRecord records[8]{}; u32 order[8]{}, heights[8]{}; u64 fenwick[8]{};
    u8 text[512]{}, draft_text[128]{}, request_text[kMessageComposerMaximumBytes]{};
    char composer_text[128]{}, source_text[128]{}; MessageDraftRecord draft_records[4]{};
    MessageStore store{}; UiVariableList list{}; MessageTimeline timeline{};
    MessageComposer composer{}, source{}; MessageDraftStore drafts{}; MessageComposerController controller{};
    const bool initialized = message_store_initialize(&store, {records, 8}, {order, 8}, {text, sizeof(text)}) &&
        ui_variable_list_initialize(&list, {heights, 8}, {fenwick, 8}, 0, 100) &&
        message_timeline_initialize(&timeline, &store, &list, 100, 100, true) &&
        message_composer_initialize(&composer, {composer_text, sizeof(composer_text)}) &&
        message_composer_initialize(&source, {source_text, sizeof(source_text)}) &&
        message_draft_store_initialize(&drafts, {draft_records, 4}, {draft_text, sizeof(draft_text)}) &&
        message_composer_controller_initialize(&controller, &composer, &drafts, &timeline, {request_text, sizeof(request_text)}) &&
        message_composer_controller_select_channel(&controller, 7).ok() &&
        message_composer_controller_set_reply(&controller, 70, false).ok();
    check(initialized, "detached send fixture initializes");
    if (!initialized) return;
    message_composer_insert_utf8(&composer, utf8_literal("draft"));
    const u32 revision = controller.reply_revision;
    message_composer_restore(&source, 8, utf8_literal("from plugin"), 11, 11);
    const auto active = message_composer_controller_queue_text(&controller, &composer, 11, 1725012296789ull, 10);
    const auto queued = message_composer_controller_queue_text(&controller, &source, 11, 1725012296789ull, 10, 80,
                                                               false, true);
    MessageComposerSubmit submit{};
    check(!active.ok() && queued.ok() && !source.bytes && composer.bytes == 5 && controller.reply_message_id == 70 &&
              controller.reply_revision == revision &&
              message_composer_controller_prepare_submit(&controller, 11, &submit).ok() && submit.channel_id == 8 &&
              submit.reply_message_id == 80 && !submit.mention_reply_author && submit.suppress_notifications &&
              utf8_equal(submit.content, utf8_literal("from plugin")),
          "detached text send carries its own reply and silence and leaves the active draft and reply alone");
    message_composer_restore(&source, 8, utf8_literal("@silent hi"), 10, 10);
    const auto prefixed = message_composer_controller_queue_text(&controller, &source, 11, 1725012296789ull, 12, 81);
    const MessageComposerRequest* second = nullptr;
    for (const auto& request : controller.requests)
        if (request.occupied && request.nonce == prefixed.nonce) second = &request;
    check(prefixed.ok() && second && second->reply_message_id == 81 && second->mention_reply_author &&
              second->suppress_notifications && second->text_bytes == 2,
          "detached text send pings the replied author by default and honours a typed @silent");
}
} // namespace
void test_message_composer_stickers(void (*check)(bool, const char*) noexcept) noexcept {
    test_reply_draft_authority(check);
    test_detached_text_send(check);
    MessageRecord records[8]{}; u32 order[8]{}, heights[8]{}; u64 fenwick[8]{};
    u8 text[512]{}, draft_text[128]{}, request_text[kMessageComposerMaximumBytes]{};
    char composer_text[128]{};
    MessageDraftRecord draft_records[4]{};
    MessageStore store{}; UiVariableList list{}; MessageTimeline timeline{};
    MessageComposer composer{}; MessageDraftStore drafts{}; MessageComposerController controller{};
    const bool initialized = message_store_initialize(&store, {records, 8}, {order, 8}, {text, sizeof(text)}) &&
        ui_variable_list_initialize(&list, {heights, 8}, {fenwick, 8}, 0, 100) &&
        message_timeline_initialize(&timeline, &store, &list, 100, 100, true) &&
        message_composer_initialize(&composer, {composer_text, sizeof(composer_text)}) &&
        message_draft_store_initialize(&drafts, {draft_records, 4}, {draft_text, sizeof(draft_text)}) &&
        message_composer_controller_initialize(&controller, &composer, &drafts, &timeline, {request_text, sizeof(request_text)});
    check(initialized, "sticker composer fixture initializes");
    if (!initialized) return;
    // Bound the fixture's admitted bank after normal initialization to force
    // real compaction with a small, fully backed request allocation.
    controller.request_text_capacity = 4;
    message_composer_reset(&composer, 7, false);
    u64 first_id = 101, second_ids[]{202, 303, 404};
    const auto first = message_composer_controller_queue_send(&controller, true, 11, 1725012296789ull, 100,
        0, {}, {}, nullptr, {&first_id, 1});
    check(message_composer_controller_set_reply(&controller, 88, false).ok(), "sticker reply acquires actual controller authority");
    const auto second = message_composer_controller_queue_send(&controller, true, 11, 1725012296790ull, 101,
        88, {}, {}, nullptr, {second_ids, 3}, false);
    first_id = 999; second_ids[0] = 999;
    MessageComposerSubmit submit{};
    const auto local = message_store_find(store, first.nonce);
    const auto* record = local.ok() ? message_store_resolve(store, local.handle) : nullptr;
    const auto local_text = record ? message_store_content(store, local.handle) : Utf8View{};
    check(first.ok() && second.ok() && first.nonce != second.nonce && controller.request_text_used == 2 &&
        record && local_text.bytes == 9 && bytes_equal(local_text.data, "[Sticker]", 9) &&
        message_composer_controller_prepare_submit(&controller, 102, &submit).ok() &&
        !submit.content.bytes && submit.sticker_count == 1 && submit.sticker_ids[0] == 101 &&
        !submit.reply_message_id && !submit.mention_reply_author,
        "sticker-only queue owns IDs, retains empty wire content, and displays local placeholder");
    check(message_composer_controller_mark_submitted(&controller, first.nonce).ok() &&
        message_composer_controller_observe_http(&controller, 400, MessageSendHttpDisposition::terminal_failure, 103).ok() &&
        message_composer_controller_discard_failed(&controller, first.nonce).ok() && controller.request_text_wasted == 1,
        "discarding sticker-only request releases its exact reserved bank slot");
    message_composer_insert_utf8(&composer, utf8_literal("abc"));
    check(message_composer_controller_set_reply(&controller, 99).ok(), "text and sticker reply acquires actual controller authority");
    const u64 third_id = 505;
    const auto third = message_composer_controller_queue_send(&controller, true, 11, 1725012296791ull, 104,
        99, {}, {}, nullptr, {&third_id, 1});
    check(third.ok() && controller.compactions == 1 && controller.request_text_used == 4 &&
        !controller.request_text_wasted && message_composer_controller_prepare_submit(&controller, 105, &submit).ok() &&
        submit.nonce == second.nonce && !submit.content.bytes && submit.sticker_count == 3 &&
        submit.sticker_ids[0] == 202 && submit.sticker_ids[1] == 303 && submit.sticker_ids[2] == 404 &&
        submit.reply_message_id == 88 && !submit.mention_reply_author,
        "sticker-only request survives bank compaction alongside a text and sticker request");
    check(message_composer_controller_mark_submitted(&controller, second.nonce).ok() &&
        message_composer_controller_observe_http(&controller, 429, MessageSendHttpDisposition::retry_wait, 106, 500).ok() &&
        message_composer_controller_select_channel(&controller, 8).ok() &&
        message_composer_controller_prepare_submit(&controller, 500, &submit).ok() &&
        submit.nonce == second.nonce && submit.channel_id == 7 && submit.sticker_count == 3 && submit.sticker_ids[0] == 202 &&
        submit.reply_message_id == 88 && !submit.mention_reply_author,
        "sticker retry preserves original nonce, channel, and IDs across channel switch");
    bool restored = false;
    check(message_timeline_reset(&timeline) && message_composer_controller_restore_local(&controller, 7, &restored) &&
        restored && store.count == 2 && controller.request_count == 2,
        "sticker outbox placeholder and text restore without requeueing");
    const u64 duplicate[]{606, 606};
    const auto requests = controller.request_count;
    check(!message_composer_controller_queue_send(&controller, true, 11, 1725012296792ull, 501,
        0, {}, {}, nullptr, {duplicate, 2}).ok() &&
        !message_composer_controller_queue_send(&controller, false, 11, 1725012296792ull, 501,
        0, {}, {}, nullptr, {&third_id, 1}).ok() && controller.request_count == requests,
        "invalid sticker selection and missing channel send permission leave queue unchanged");
    check(message_composer_controller_mark_submitted(&controller, second.nonce).ok() &&
        message_composer_controller_observe_http(&controller, 400, MessageSendHttpDisposition::terminal_failure, 502).ok() &&
        message_composer_controller_retry(&controller, second.nonce, 503).ok() &&
        message_composer_controller_prepare_submit(&controller, 503, &submit).ok() &&
        submit.nonce == second.nonce && submit.reply_message_id == 88 && !submit.mention_reply_author,
        "manual retry preserves original reply target and disabled author mention after composer and channel changes");
    check(message_composer_controller_mark_submitted(&controller, second.nonce).ok() &&
        message_composer_controller_observe_http(&controller, 400, MessageSendHttpDisposition::terminal_failure, 504).ok() &&
        message_composer_controller_discard_failed(&controller, second.nonce).ok() &&
        message_composer_controller_prepare_submit(&controller, 505, &submit).ok() && submit.nonce == third.nonce &&
        submit.content.bytes == 3 && bytes_equal(submit.content.data, "abc", 3) &&
        submit.sticker_count == 1 && submit.sticker_ids[0] == 505 &&
        submit.reply_message_id == 99 && submit.mention_reply_author,
        "text and sticker submit retains actual text after earlier sticker request is discarded");
    // Separate fixture state from the preceding ordinary sends.
    check(message_timeline_reset(&timeline) && message_composer_controller_initialize(&controller,
        &composer, &drafts, &timeline, {request_text, sizeof(request_text)}), "forward outbox fixture resets");
    message_composer_reset(&composer, 7, false);
    composer.focused = true;
    message_composer_insert_utf8(&composer, utf8_literal("active draft"));
    message_composer_controller_commit_draft(&controller);
    char caption[]{'n', 'o', 't', 'e'};
    const auto forward = message_composer_controller_queue_forward(&controller, 8, {caption, 4}, {80, 7, 6},
        11, 1725012296793ull, 600);
    const auto second_forward = message_composer_controller_queue_forward(&controller, 9, {}, {80, 7, 6},
        11, 1725012296793ull, 601);
    caption[0] = 'x';
    MessageDraftSnapshot active_draft{};
    check(forward.ok() && second_forward.ok() && forward.nonce != second_forward.nonce && !store.count &&
        composer.channel_id == 7 && composer.focused && message_composer_text(composer).bytes == 12 &&
        message_draft_store_snapshot(drafts, 7, &active_draft) && active_draft.text.bytes == 12 &&
        message_composer_controller_prepare_submit(&controller, 602, &submit).ok() &&
        submit.channel_id == 8 && submit.content.bytes == 4 && bytes_equal(submit.content.data, "note", 4) &&
        submit.forward_reference.message_id == 80 && submit.forward_reference.channel_id == 7 &&
        submit.forward_reference.guild_id == 6 && !submit.reply_message_id && !submit.mention_reply_author &&
        !submit.attachment_count && !submit.sticker_count,
        "forward owns caption and origin per destination without changing active draft or timeline");
    check(message_composer_controller_mark_submitted(&controller, forward.nonce).ok() &&
        !message_composer_controller_cancel_unsent(&controller, forward.nonce).ok() &&
        message_composer_controller_observe_http(&controller, 429, MessageSendHttpDisposition::retry_wait, 603, 700).ok() &&
        message_composer_controller_prepare_submit(&controller, 700, &submit).ok() && submit.nonce == forward.nonce &&
        submit.channel_id == 8 && submit.forward_reference.message_id == 80 &&
        message_composer_controller_mark_submitted(&controller, forward.nonce).ok() &&
        message_composer_controller_observe_http(&controller, 403, MessageSendHttpDisposition::terminal_failure, 701).ok() &&
        message_composer_controller_retry(&controller, forward.nonce, 702).ok() &&
        message_composer_controller_prepare_submit(&controller, 702, &submit).ok() &&
        submit.nonce == forward.nonce && submit.forward_reference.channel_id == 7 && submit.forward_reference.guild_id == 6,
        "forward automatic and manual retry preserve nonce and source while inflight cancellation is refused");
    check(message_composer_controller_cancel_unsent(&controller, forward.nonce).ok() &&
        controller.request_count == 1 && controller.sends.active_count == 1 &&
        message_composer_controller_prepare_submit(&controller, 703, &submit).ok() &&
        submit.nonce == second_forward.nonce && submit.channel_id == 9 && !submit.content.bytes &&
        submit.forward_reference.message_id == 80 &&
        !message_composer_controller_queue_forward(&controller, 8, {}, {0, 7, 6}, 11, 1725012296793ull, 704).ok() &&
        controller.request_count == 1,
        "unsent forward cancellation releases only its request and preserves empty-caption sibling");
    bool forward_restored = false;
    check(message_composer_controller_restore_local(&controller, 9, &forward_restored) && forward_restored && store.count == 1,
        "destination outbox can restore empty forward preview without fabricating a server snapshot");
    check(message_composer_controller_cancel_unsent(&controller, second_forward.nonce).ok() &&
        !store.count && !controller.request_count && !controller.sends.active_count &&
        message_composer_controller_prepare_submit(nullptr, 705, &submit).error == MessageComposerControllerError::invalid_input &&
        !submit.forward_reference.message_id && !submit.forward_reference.channel_id && !submit.forward_reference.guild_id,
        "cancelling last unsent forward removes restored preview and failed preparation clears source authority");
}
} // namespace maicord
