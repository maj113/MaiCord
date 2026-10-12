#include "maicord/app/pinned_message_controller.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
MessagePinsCursor cursor(Utf8View text) noexcept {
    MessagePinsCursor result{};
    if (text.bytes <= sizeof(result.timestamp)) {
        copy_bytes(result.timestamp, text.data, text.bytes);
        result.bytes = static_cast<u8>(text.bytes);
    }
    return result;
}
bool same_cursor(const MessagePinsCursor& a, const MessagePinsCursor& b) noexcept {
    return a.bytes == b.bytes && bytes_equal(a.timestamp, b.timestamp, a.bytes);
}
MessagePin pin(u64 id, MessagePinsCursor when, u64 milliseconds) noexcept {
    MessagePin result{};
    result.message.id = id;
    result.message.channel_id = 4;
    result.message.author_id = 8;
    result.message.timestamp_ms = 1000 + id;
    result.message.author_name = utf8_literal("Alice");
    result.message.author_avatar_hash = utf8_literal("a_0123456789abcdef0123456789abcdef");
    result.message.content = utf8_literal("Pinned text");
    result.pinned_at = when;
    result.pinned_at_ms = milliseconds;
    return result;
}
bool submit(PinnedMessageController& value, u64 id, u64 now = 100) noexcept {
    return pinned_message_controller_submitted(&value, pinned_message_controller_request(value, now), id);
}
bool complete(PinnedMessageController& value, u64 id, PinnedMessageCompletionKind kind,
              const MessagePinsPage* page = nullptr, ConstSlice<MessagePin> items = {},
              u64 retry_at = 0, u64 now = 100) noexcept {
    return pinned_message_controller_complete(&value, value.scope, id, {kind, retry_at}, page, items, now);
}
} // namespace

void test_pinned_message_controller(void (*check)(bool, const char*) noexcept) noexcept {
    {
        PinnedMessageController forwarded{};
        const PinnedMessageScope scoped{1, 2, 3, 4, 5};
        const auto when = cursor(utf8_literal("2026-10-05T12:00:00.123456+00:00"));
        auto forwarded_item = pin(90, when, 3000);
        char outer[] = "caption";
        char quoted[] = "quoted <@3> child";
        forwarded_item.message.content = {outer, sizeof(outer) - 1};
        forwarded_item.message.reference.type = 1;
        forwarded_item.message.reference.message_id = 800;
        forwarded_item.message.snapshot.present = true;
        forwarded_item.message.snapshot.content = {quoted, sizeof(quoted) - 1};
        forwarded_item.message.snapshot.attachments.count = 2;
        forwarded_item.message.snapshot.embeds.count = 1;
        const MessagePinsPage page{4, when, 1, 0, false};
        const bool loaded = pinned_message_controller_open(&forwarded, scoped, true, true) &&
            submit(forwarded, 777) && complete(forwarded, 777, PinnedMessageCompletionKind::success,
                                             &page, {&forwarded_item, 1});
        clear_bytes(outer, sizeof(outer)); clear_bytes(quoted, sizeof(quoted));
        const auto* result = pinned_message_controller_row(forwarded, 0);
        check(loaded && result && result->message_id == 90 && result->channel_id == 4 && result->author_id == 8 &&
            utf8_equal(pinned_message_content(*result), utf8_literal("caption\nForwarded message\n> quoted <@3> child")) &&
            result->attachment_count == 2 && result->embed_count == 1 && result->forwarded &&
            pinned_message_controller_select(&forwarded, 90) && !pinned_message_controller_select(&forwarded, 800),
            "pins_forwarded_card_owns_both_body_excerpts_and_child_media_summary_while_jump_authority_stays_outer");
    }
    PinnedMessageController value{};
    const PinnedMessageScope scope{1, 2, 3, 4, 5};
    const auto newest = cursor(utf8_literal("2026-10-05T12:00:00.123456+00:00"));
    const auto older = cursor(utf8_literal("2026-10-04T12:00:00.000001+00:00"));
    const auto oldest = cursor(utf8_literal("2026-10-03T12:00:00.000000+00:00"));
    MessagePin items[]{pin(10, newest, 3000), pin(9, older, 2000)};
    const MessagePinsPage first{4, older, 2, 0, true};
    const MessagePinsPage last{4, oldest, 1, 0, false};
    MessagePin last_item[]{pin(8, oldest, 1000)};
    check(pinned_message_controller_open(&value, scope, true, false) &&
        value.phase == PinnedMessagePhase::loading && pinned_message_controller_next_wake(value) == 1 &&
        !pinned_message_controller_mutate(&value, 10, false) && submit(value, 1) &&
        complete(value, 1, PinnedMessageCompletionKind::success, &first, {items, 2}) &&
        value.has_page && value.authoritative && value.page.count == 2 && !value.has_previous &&
        pinned_message_controller_select(&value, 9) && !pinned_message_controller_select(&value, 99),
        "pins_read_permission_loads_complete_page_and_jump_accepts_only_visible_message");
    check(pinned_message_controller_next(&value) && value.page_index == 0 &&
        value.selected_message_id == 9 && same_cursor(value.pending.query.before, older) &&
        value.requested_page_index == 1 && submit(value, 2) &&
        complete(value, 2, PinnedMessageCompletionKind::success, &last, {last_item, 1}) &&
        value.page_index == 1 && value.has_previous && value.page.count == 1 &&
        !value.selected_message_id && !pinned_message_controller_next(&value),
        "pins_older_navigation_preserves_current_page_until_exact_cursor_receipt_then_replaces_complete_page");
    check(pinned_message_controller_previous(&value) && !value.pending.query.before.bytes &&
        value.page_index == 1 && submit(value, 3) &&
        complete(value, 3, PinnedMessageCompletionKind::success, &first, {items, 2}) &&
        value.page_index == 0 && !value.has_previous && value.cursor_count == 1,
        "pins_newer_navigation_uses_copied_history_and_returns_to_first_page");

    char author[]{'A', 'l', 'i', 'c', 'e'};
    char content[kPinnedMessageExcerptBytes + 4];
    for (u32 at = 0; at < kPinnedMessageExcerptBytes - 1; ++at) content[at] = 'a';
    content[kPinnedMessageExcerptBytes - 1] = static_cast<char>(0xf0);
    content[kPinnedMessageExcerptBytes] = static_cast<char>(0x9f);
    content[kPinnedMessageExcerptBytes + 1] = static_cast<char>(0x9a);
    content[kPinnedMessageExcerptBytes + 2] = static_cast<char>(0x80);
    items[0].message.author_name = {author, sizeof(author)};
    items[0].message.content = {content, kPinnedMessageExcerptBytes + 3};
    items[0].message.attachments.count = 2;
    items[0].message.stickers.count = 1;
    items[0].message.reference.type = 1;
    items[0].message.rich_truncations = 1;
    items[1].message.content = {};
    items[1].message.type = 6;
    check(pinned_message_controller_refresh(&value) && submit(value, 4) &&
        complete(value, 4, PinnedMessageCompletionKind::success, &first, {items, 2}),
        "pins_reload_accepts_text_and_media_only_messages");
    author[0] = 'X'; content[0] = 'X';
    const auto* row = pinned_message_controller_row(value, 0);
    const auto* media_only = pinned_message_controller_row(value, 1);
    check(row && row->author[0] == 'A' && row->content[0] == 'a' &&
        row->content_bytes <= kPinnedMessageExcerptBytes && row->excerpt_truncated &&
        row->content_bytes >= 17 && bytes_equal(row->content + row->content_bytes - 17, "Forwarded message", 17) &&
        utf8_validate(pinned_message_content(*row)).valid && row->attachment_count == 2 &&
        row->sticker_count == 1 && row->forwarded && row->rich_truncated &&
        media_only && !media_only->content_bytes && media_only->type == 6 && !pinned_message_controller_row(value, 2),
        "pins_excerpts_own_transient_text_preserve_media_summary_and_never_split_utf8");
    pinned_message_controller_authority(&value, scope, true, true);
    check(!pinned_message_controller_mutate(&value, 9, false) &&
        !pinned_message_controller_mutate(&value, 9, true) && !value.queued && value.authoritative &&
        pinned_message_controller_select(&value, 9),
        "pins_type_six_system_notification_remains_jumpable_but_cannot_be_pinned_or_unpinned");
    items[0] = pin(10, newest, 3000);
    items[1] = pin(9, older, 2000);

    check(pinned_message_controller_next(&value) && submit(value, 5), "pins_failed_page_fixture_submits");
    MessagePin invalid[]{last_item[0], last_item[0]};
    MessagePinsPage invalid_page{4, oldest, 2, 0, false};
    check(complete(value, 5, PinnedMessageCompletionKind::success, &invalid_page, {invalid, 2}) &&
        value.phase == PinnedMessagePhase::failed && value.failure == PinnedMessageFailure::response &&
        value.page_index == 0 && value.page.count == 2 && value.rows[0].message_id == 10 &&
        pinned_message_controller_retry(&value) && same_cursor(value.pending.query.before, older) &&
        submit(value, 6) && complete(value, 6, PinnedMessageCompletionKind::success, &last, {last_item, 1}),
        "pins_malformed_page_fails_atomically_and_retry_keeps_requested_cursor");

    pinned_message_controller_authority(&value, scope, true, true);
    check(pinned_message_controller_mutate(&value, 777, true) &&
        pinned_message_controller_mutation_pending(value) && !value.authoritative &&
        !pinned_message_controller_refresh(&value) && !pinned_message_controller_next(&value) &&
        value.pending.operation == MessagePinsOperation::pin && submit(value, 7) &&
        complete(value, 7, PinnedMessageCompletionKind::success) && value.queued &&
        value.pending.operation == MessagePinsOperation::fetch && !value.pending.query.before.bytes &&
        value.page_index == 1 && value.rows[0].message_id == 8 &&
        submit(value, 8) && complete(value, 8, PinnedMessageCompletionKind::success, &first, {items, 2}) &&
        value.page_index == 0 && value.cursor_count == 1 && !value.has_previous,
        "pins_timeline_pin_locks_overlapping_actions_and_repages_first_page_without_optimistic_rows");
    check(pinned_message_controller_mutate(&value, 10, false) && submit(value, 9) &&
        complete(value, 9, PinnedMessageCompletionKind::ambiguous) && value.queued &&
        value.pending.operation == MessagePinsOperation::fetch &&
        value.failure == PinnedMessageFailure::ambiguous && submit(value, 10) &&
        complete(value, 10, PinnedMessageCompletionKind::success, &first, {items, 2}),
        "pins_ambiguous_unpin_reconciles_with_authoritative_fetch_instead_of_blind_mutation_retry");
    check(pinned_message_controller_mutate(&value, 10, false) && submit(value, 11) &&
        complete(value, 11, PinnedMessageCompletionKind::permission_lost) && !value.can_pin &&
        value.permission_denied && value.authoritative && pinned_message_controller_select(&value, 10) &&
        !pinned_message_controller_retry(&value),
        "pins_server_pin_denial_preserves_readable_page_and_jump_but_disables_mutation_retry");
    pinned_message_controller_authority(&value, scope, true, true);
    check(!value.can_pin, "pins_stale_permission_projection_cannot_clear_server_denial");
    pinned_message_controller_reauthorize(&value, scope, true, true);
    check(value.can_pin && pinned_message_controller_mutate(&value, 10, false) &&
        pinned_message_controller_invalidate(&value, scope) && value.queued &&
        value.pending.operation == MessagePinsOperation::unpin && submit(value, 12) &&
        complete(value, 12, PinnedMessageCompletionKind::rejected) && !value.authoritative &&
        value.queued && value.failure == PinnedMessageFailure::rejected && submit(value, 120) &&
        complete(value, 120, PinnedMessageCompletionKind::success, &first, {items, 2}),
        "pins_fresh_pin_permission_authority_restores_actions_and_invalidation_keeps_unsent_mutation");

    check(pinned_message_controller_refresh(&value) && submit(value, 13) &&
        pinned_message_controller_invalidate(&value, scope) &&
        complete(value, 13, PinnedMessageCompletionKind::success, &last, {last_item, 1}) &&
        value.queued && !value.authoritative && value.rows[0].message_id == 10 &&
        !pinned_message_controller_select(&value, 10) && submit(value, 14) &&
        complete(value, 14, PinnedMessageCompletionKind::success, &first, {items, 2}),
        "pins_gateway_invalidation_discards_stale_fetch_receipt_and_repages_before_reenabling_jump");

    check(pinned_message_controller_refresh(&value) && submit(value, 15) &&
        complete(value, 15, PinnedMessageCompletionKind::rate_limited, nullptr, {}, 500, 100) &&
        pinned_message_controller_next_wake(value) == 500 &&
        !pinned_message_controller_request(value, 499).ready &&
        pinned_message_controller_request(value, 500).ready,
        "pins_rate_limit_retry_honors_authoritative_future_deadline");
    check(submit(value, 16, 500) && complete(value, 16, PinnedMessageCompletionKind::rate_limited,
        nullptr, {}, 600, 500) && submit(value, 17, 600) &&
        complete(value, 17, PinnedMessageCompletionKind::rate_limited, nullptr, {}, 700, 600) &&
        submit(value, 18, 700) && complete(value, 18, PinnedMessageCompletionKind::rate_limited,
        nullptr, {}, 800, 700) && !value.queued && value.phase == PinnedMessagePhase::failed &&
        !pinned_message_controller_next_wake(value),
        "pins_rate_limit_attempts_are_bounded_without_busy_timer_loop");
    check(pinned_message_controller_retry(&value) && submit(value, 19) &&
        complete(value, 19, PinnedMessageCompletionKind::rate_limited) && !value.queued &&
        value.phase == PinnedMessagePhase::failed,
        "pins_rate_limit_without_deadline_requires_explicit_retry");

    check(pinned_message_controller_refresh(&value), "pins_admission_fixture_reloads");
    auto request = pinned_message_controller_request(value, 100);
    check(pinned_message_controller_admission_failed(&value, request, true) && value.queued &&
        pinned_message_controller_request(value, 100).ready &&
        pinned_message_controller_admission_failed(&value, request, false) &&
        value.phase == PinnedMessagePhase::failed && !value.queued &&
        !pinned_message_controller_submitted(&value, request, 20),
        "pins_busy_admission_retains_work_and_terminal_admission_retires_exact_queued_revision");

    check(pinned_message_controller_retry(&value) && submit(value, 21), "pins_stale_scope_fixture_submits");
    auto different = scope; ++different.channel_generation;
    check(!pinned_message_controller_complete(&value, different, 21,
        {PinnedMessageCompletionKind::success}, &first, {items, 2}, 100) &&
        !complete(value, 22, PinnedMessageCompletionKind::success, &first, {items, 2}) && value.request_id == 21,
        "pins_wrong_channel_generation_or_request_id_cannot_consume_ticket");
    pinned_message_controller_authority(&value, different, true, true);
    check(!value.active && value.request_id == 21 && !pinned_message_controller_row(value, 0) &&
        !pinned_message_controller_open(&value, different, true, true) &&
        complete(value, 21, PinnedMessageCompletionKind::success, &first, {items, 2}) &&
        !value.request_id && !value.active && pinned_message_controller_open(&value, different, true, true),
        "pins_scope_retirement_drains_exact_ticket_before_rebind_and_never_publishes_late_rows");
    request = pinned_message_controller_request(value, 100);
    auto changed_request = request; ++changed_request.scope.account_scope;
    check(!pinned_message_controller_submitted(&value, changed_request, 23) &&
        !pinned_message_controller_admission_failed(&value, changed_request, false) && submit(value, 23),
        "pins_account_epoch_fences_admission_before_shared_rest_owner_submission");
    pinned_message_controller_cancel(&value);
    check(complete(value, 23, PinnedMessageCompletionKind::cancelled) && !value.request_id &&
        !pinned_message_controller_next_wake(value), "pins_closed_panel_consumes_cancelled_receipt_without_rescheduling");

    check(pinned_message_controller_open(&value, scope, true, true) &&
        pinned_message_controller_mutate(&value, 555, true) &&
        value.pending.operation == MessagePinsOperation::pin && submit(value, 24) &&
        complete(value, 24, PinnedMessageCompletionKind::success) && value.queued,
        "pins_context_action_can_replace_unsent_initial_fetch_and_reconciles_after_acknowledgment");
    check(submit(value, 25), "pins_read_permission_loss_fixture_submits");
    pinned_message_controller_authority(&value, scope, false, false);
    check(complete(value, 25, PinnedMessageCompletionKind::success, &first, {items, 2}) &&
        !value.available && !value.authoritative && !pinned_message_controller_row(value, 0) &&
        !pinned_message_controller_request(value, 100).ready,
        "pins_read_permission_loss_hides_rows_and_blocks_receipt_publication");
    pinned_message_controller_authority(&value, scope, true, true);
    check(value.queued && submit(value, 26) &&
        complete(value, 26, PinnedMessageCompletionKind::success, &first, {items, 2}),
        "pins_restored_read_authority_fetches_fresh_page");
    check(pinned_message_controller_refresh(&value) && submit(value, 27) &&
        complete(value, 27, PinnedMessageCompletionKind::permission_lost),
        "pins_server_read_permission_denial_fixture_completes");
    pinned_message_controller_authority(&value, scope, true, true);
    check(value.read_denied && !value.available && !value.queued &&
        !pinned_message_controller_next_wake(value),
        "pins_stale_read_permission_projection_cannot_loop_server_denied_fetches");
    pinned_message_controller_reauthorize(&value, scope, true, true);
    check(!value.read_denied && value.queued && submit(value, 28) &&
        complete(value, 28, PinnedMessageCompletionKind::success, &first, {items, 2}),
        "pins_fresh_read_projection_clears_server_denial_and_fetches_again");

    value.page_index = kPinnedMessageCursorHistory - 1;
    value.cursor_count = kPinnedMessageCursorHistory;
    value.has_previous = true;
    check(!pinned_message_controller_next(&value) && value.failure == PinnedMessageFailure::capacity &&
        value.page.count == 2 && value.rows[0].message_id == 10 && !value.queued,
        "pins_cursor_history_exhaustion_is_explicit_and_preserves_complete_last_page");
    value.revision = ~u64{0};
    check(!pinned_message_controller_request(value, 100).ready &&
        !pinned_message_controller_refresh(&value) && !pinned_message_controller_mutate(&value, 10, true) &&
        !pinned_message_controller_open(&value, scope, true, true),
        "pins_revision_exhaustion_cannot_reuse_old_admission_or_jump_authority");
    pinned_message_controller_cancel(&value);
    check(value.phase == PinnedMessagePhase::retired && value.failure == PinnedMessageFailure::exhausted &&
        !value.active, "pins_exhausted_owner_remains_retired");
}
} // namespace maicord
