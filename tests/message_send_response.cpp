#include "maicord/app/message_send_response.hpp"
#include "maicord/app/message_history_sync.hpp"

namespace maicord {
void test_message_send_response(void (*check)(bool, const char*) noexcept) noexcept {
    MessageRecord records[4]{}; u32 order[4]{}, heights[4]{}; u64 fenwick[4]{};
    u8 text[512]{}, draft_text[128]{}, request_text[kMessageComposerMaximumBytes]{}, rich_text[4096]{};
    char composer_text[128]{}, content[2048]{};
    MessageDraftRecord draft_records[2]{};
    MessageRichRecord rich_records[4]{}; u16 rich_order[4]{};
    MessageStore store{}; MessageRichStore rich{}; UiVariableList list{}; MessageTimeline timeline{};
    MessageComposer composer{}; MessageDraftStore drafts{}; MessageComposerController controller{};
    u32 structurals[256]{}; GatewayMessageEvent messages[1]{};
    GatewayMessageAttachment attachments[2]{};
    GatewayMessageAttachment snapshot_attachments[2]{};
    GatewayMessageNamedUser snapshot_users[4]{};
    GatewayMessageNamedUser users[4]{};
    u64 member_roles[4]{};
    const MessageHistoryDecodeScratch scratch{{}, {structurals, 256}, {content, sizeof(content)}, {messages, 1},
        {users, 4}, {{}, {attachments, 2}, {}, {}, {}, {}, {member_roles, 4},
            {{snapshot_attachments, 2}, {}, {}, {}, {}, {snapshot_users, 4}}}, 32};
    const auto reset = [&]() noexcept {
        const bool ok = message_store_initialize(&store, {records, 4}, {order, 4}, {text, sizeof(text)}) &&
            ui_variable_list_initialize(&list, {heights, 4}, {fenwick, 4}, 0, 100) &&
            message_timeline_initialize(&timeline, &store, &list, 100, 100, true) &&
            message_rich_store_initialize(&rich, {rich_records, 4}, {rich_order, 4}, {rich_text, sizeof(rich_text)}) &&
            message_timeline_bind_rich_store(&timeline, &rich) &&
            message_composer_initialize(&composer, {composer_text, sizeof(composer_text)}) &&
            message_draft_store_initialize(&drafts, {draft_records, 2}, {draft_text, sizeof(draft_text)}) &&
            message_composer_controller_initialize(&controller, &composer, &drafts, &timeline, {request_text, sizeof(request_text)});
        message_composer_reset(&composer, 7, false);
        return ok;
    };
    const auto queue = [&]() noexcept {
        message_composer_insert_utf8(&composer, utf8_literal("local"));
        const auto result = message_composer_controller_queue_send(&controller, true, 11, 1725012296789ull, 100);
        if (!result.ok() || !message_composer_controller_mark_submitted(&controller, result.nonce).ok()) return u64{0};
        return result.nonce;
    };
    constexpr auto body = utf8_literal(R"({"id":"900","channel_id":"7","author":{"id":"11","username":"self"},"content":"server","timestamp":"2024-08-30T12:00:00Z","attachments":[{"id":"901","filename":"image.png","size":64,"url":"https://cdn.discordapp.com/attachments/7/901/image.png","width":32,"height":32}]})");
    RestClientResponse response{}; response.status_code = 200; response.body_bytes = body.bytes;
    MessageSendCompletion completion{};
    check(reset(), "send response fixture initializes");
    u64 nonce = queue();
    auto accepted = message_send_accept_response(&controller, response, body, scratch, 7, 11, 110);
    const auto found = message_store_find(store, 900);
    const auto* record = found.ok() ? message_store_resolve(store, found.handle) : nullptr;
    const auto* detail = message_rich_store_find(rich, 900);
    check(nonce && accepted.http.ok() && accepted.body_decoded && accepted.message.ok() && accepted.message.changed &&
        record && record->delivery == MessageDeliveryState::authoritative && store.count == 1 &&
        !message_store_find(store, nonce).ok() && utf8_equal(message_store_content(store, found.handle), utf8_literal("server")) &&
        detail && detail->attachment_count == 1 && detail->attachments[0].size == 64 && detail->attachments[0].width == 32 &&
        message_rich_store_string(rich, detail->attachments[0].url).bytes &&
        message_composer_controller_take_completion(&controller, &completion) && completion.response_observed &&
        completion.http_observed && completion.api_elapsed_ms == 10 && !completion.gateway_observed && !completion.history_observed &&
        !controller.request_count && !controller.inflight_nonce,
        "successful POST installs authoritative text and attachments and confirms delivery without Gateway or nonce echo");
    for (auto& byte : content) byte = 'x';
    check(utf8_equal(message_store_content(store, found.handle), utf8_literal("server")) && detail &&
        message_rich_store_string(rich, detail->attachments[0].url).data[0] == 'h',
        "authoritative response text and attachment URL are owned after all decode text is overwritten");

    check(reset(), "forwarded send response fixture resets"); nonce = queue();
    constexpr auto forwarded_body = utf8_literal(R"({"id":"900","channel_id":"7","author":{"id":"11"},"content":"outer note","timestamp":"2024-08-30T12:00:00Z","message_reference":{"type":1,"message_id":"800","channel_id":"8"},"attachments":[{"id":"901","filename":"outer.png","size":64,"url":"https://example.com/outer.png"}],"message_snapshots":[{"message":{"content":"child body <@11>","mentions":[{"id":"11","username":"child mention"}],"attachments":[{"id":"902","filename":"child.png","size":64,"url":"https://example.com/child.png"}]}}]})");
    response.body_bytes = forwarded_body.bytes;
    accepted = message_send_accept_response(&controller, response, forwarded_body, scratch, 7, 11, 110);
    const auto* forwarded_child = message_rich_store_snapshot(rich, 900);
    check(nonce && accepted.body_decoded && accepted.message.ok() && accepted.message.changed &&
        messages[0].snapshot.present && messages[0].attachments.count == 1 && messages[0].snapshot.attachments.count == 1 &&
        messages[0].snapshot.attachments.data == snapshot_attachments && messages[0].snapshot.mention_users.data == snapshot_users &&
        !messages[0].mentions_current_user && messages[0].snapshot.mention_users.count == 1 &&
        utf8_equal(messages[0].snapshot.content, utf8_literal("child body <@11>")) &&
        forwarded_child && forwarded_child->attachment_count == 1 &&
        message_composer_controller_take_completion(&controller, &completion) && completion.response_observed,
        "send confirmation forwards independent child scratch and installs snapshot without child notification pings");
    for (auto& byte : content) byte = 'x';
    check(forwarded_child && utf8_equal(message_rich_store_string(rich, forwarded_child->snapshot.content), utf8_literal("child body <@11>")) &&
        utf8_equal(message_rich_store_string(rich, forwarded_child->attachments[0].filename), utf8_literal("child.png")),
        "forwarded send response child text and media survive retirement of decode scratch");
    response.body_bytes = body.bytes;

    const Utf8View rejected_bodies[] = {
        utf8_literal(R"({"id":"900","channel_id":"8","author":{"id":"11"},"content":"wrong channel","timestamp":"2024-08-30T12:00:00Z"})"),
        utf8_literal(R"({"id":"900","channel_id":"7","author":{"id":"12"},"content":"wrong author","timestamp":"2024-08-30T12:00:00Z"})"),
        utf8_literal(R"({"id":"900","channel_id":"7","author":{"id":"11"},"nonce":"123","content":"wrong nonce","timestamp":"2024-08-30T12:00:00Z"})"),
        utf8_literal(R"({"id":"900","channel_id":"7","author":{"id":"11"},"content":"truncated")"),
        utf8_literal(R"({"id":"900","channel_id":"7","author":{"id":"11"},"content":"valid prefix","timestamp":"2024-08-30T12:00:00Z"} {})"),
        {},
    };
    for (const auto invalid : rejected_bodies) {
        check(reset(), "invalid response fixture resets"); nonce = queue();
        response.body_bytes = invalid.bytes;
        accepted = message_send_accept_response(&controller, response, invalid, scratch, 7, 11, 110);
        check(nonce && accepted.http.ok() && accepted.body_expected &&
            (!accepted.body_decoded || !accepted.message.ok()) && controller.request_count == 1 &&
            !controller.inflight_nonce && message_store_find(store, nonce).ok() && !message_store_find(store, 900).ok() &&
            !message_composer_controller_take_completion(&controller, &completion),
            "invalid or mismatched success body keeps the owned send for Gateway recovery without a duplicate submission");
    }

    response.body_bytes = body.bytes;
    check(reset(), "inactive channel fixture resets"); nonce = queue();
    check(message_timeline_reset(&timeline), "navigation clears old resident view");
    accepted = message_send_accept_response(&controller, response, body, scratch, 8, 11, 110);
    check(nonce && accepted.message.ok() && accepted.message.changed && !store.count &&
        message_composer_controller_take_completion(&controller, &completion) && completion.response_observed && !controller.request_count,
        "response confirms its original channel without inserting into the newly selected channel");

    check(reset(), "older window response fixture resets"); nonce = queue();
    check(message_timeline_reset(&timeline), "older window replaces local projections");
    GatewayMessageEvent old{}; old.channel_id = 7; old.author_id = 12; old.timestamp_ms = 1725012296000ull;
    old.content = utf8_literal("older");
    old.fields = gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_author_id |
        gateway_message_field_timestamp | gateway_message_field_content;
    for (u32 i = 0; i < 4; ++i) { old.id = 10 + i; check(message_timeline_apply_created(&timeline, old).ok(), "older window fills"); }
    check(message_timeline_set_scroll(&timeline, 0, false), "reader remains in older history");
    accepted = message_send_accept_response(&controller, response, body, scratch, 7, 11, 110);
    check(accepted.message.ok() && accepted.message.changed && store.count == 4 && message_store_find(store, 10).ok() &&
        !message_store_find(store, 900).ok() && timeline.newer_refetch_required &&
        message_composer_controller_take_completion(&controller, &completion) && completion.response_observed,
        "send confirmation preserves a full older window and schedules newest-history recovery");

    check(reset(), "attachment-only response fixture resets");
    const UploadedAttachment upload{utf8_literal("image.png"), utf8_literal("upload/image.png"), {}, 0, false};
    const u64 size = 64;
    const auto file_send = message_composer_controller_queue_send(&controller, true, 11, 1725012296789ull, 100, 0, {&upload, 1}, {&size, 1});
    constexpr auto file_body = utf8_literal(R"({"id":"900","channel_id":"7","author":{"id":"11"},"content":"","timestamp":"2024-08-30T12:00:00Z","attachments":[{"id":"901","filename":"image.png","size":64,"url":"https://cdn.discordapp.com/attachments/7/901/image.png"}]})");
    response.body_bytes = file_body.bytes;
    check(file_send.ok() && message_composer_controller_mark_submitted(&controller, file_send.nonce).ok(), "attachment-only POST is submitted");
    accepted = message_send_accept_response(&controller, response, file_body, scratch, 7, 11, 110);
    check(accepted.body_decoded && accepted.message.ok() && accepted.message.changed && rich.count == 1 &&
        !message_rich_store_find(rich, file_send.nonce) && message_rich_store_find(rich, 900) &&
        message_composer_controller_take_completion(&controller, &completion) && completion.response_observed &&
        !controller.request_count && !controller.request_text_used,
        "attachment-only POST replaces the local card and releases its queued upload reference");
    response.body_bytes = body.bytes;

    for (u32 deleted = 0; deleted < 2; ++deleted) {
        check(reset(), "Gateway precedence fixture resets"); nonce = queue();
        check(message_send_tracker_observe_gateway(&controller.sends, &timeline, nonce, 7, 11, 900, 105).ok(),
            "Gateway confirms before the HTTP response");
        GatewayMessageEvent newer{}; newer.id = 900; newer.channel_id = 7; newer.author_id = 11;
        newer.timestamp_ms = 1725012296789ull; newer.content = utf8_literal("newer edit");
        newer.fields = gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_author_id |
            gateway_message_field_timestamp | gateway_message_field_content;
        check(message_timeline_apply_created(&timeline, newer).ok(), "newer Gateway content enters shared model");
        if (deleted) check(message_timeline_reset(&timeline), "message deleted before POST response");
        accepted = message_send_accept_response(&controller, response, body, scratch, 7, 11, 110);
        const auto resident = message_store_find(store, 900);
        check(accepted.http.ok() && accepted.message.ok() && !accepted.message.changed &&
            (deleted ? !store.count : resident.ok() && utf8_equal(message_store_content(store, resident.handle), utf8_literal("newer edit"))) &&
            message_composer_controller_take_completion(&controller, &completion) && completion.gateway_observed && !completion.response_observed,
            "late POST body cannot overwrite a Gateway edit or resurrect a deleted message");
    }

    for (u32 take_failed = 0; take_failed < 2; ++take_failed) {
        check(reset(), "queued Gateway completion fixture resets"); nonce = queue(); bool changed = false;
        check(message_send_tracker_observe_gateway(&controller.sends, &timeline, nonce, 7, 11, 900, 105).ok() &&
            message_composer_controller_tick(&controller, 100 + kMessageSendTrackerTimeoutMs, &changed),
            "Gateway observation reaches deadline before HTTP receipt");
        const bool drained = take_failed ? message_composer_controller_submit_failed(&controller, nonce, 30200).ok() :
            message_send_accept_response(&controller, response, body, scratch, 7, 11, 30200).http.ok();
        check(drained && !controller.inflight_nonce && !store.count &&
            message_composer_controller_take_completion(&controller, &completion) && completion.gateway_observed &&
            !completion.response_observed && !controller.request_count,
            "late success or response-take failure drains an unconsumed Gateway completion without resurrection");
    }

    check(reset(), "rate limit fixture resets"); nonce = queue();
    response.status_code = 429; response.rate_limited = true; response.retry_at_ms = 500;
    accepted = message_send_accept_response(&controller, response, body, scratch, 7, 11, 110);
    MessageComposerSubmit submit{};
    check(nonce && accepted.http.ok() && !accepted.body_expected && controller.request_count == 1 &&
        message_composer_controller_prepare_submit(&controller, 499, &submit).error == MessageComposerControllerError::not_found &&
        message_composer_controller_prepare_submit(&controller, 500, &submit).ok() && submit.nonce == nonce,
        "429 response body cannot confirm delivery and keeps the server retry deadline");

    constexpr ApiRejection rejections[] = {{0, false, true}, {40002, true, false}, {40004, true, false}, {20026, true, false}};
    for (const auto rejection : rejections) {
        check(reset(), "verification rejection fixture resets"); nonce = queue();
        response.rejection = rejection;
        // Even a response carrying a rate deadline must not replay a challenge.
        response.status_code = 429; response.rate_limited = true; response.retry_at_ms = 500;
        accepted = message_send_accept_response(&controller, response, {}, scratch, 7, 11, 110);
        const auto local = message_store_find(store, nonce);
        check(nonce && accepted.http.ok() && !accepted.body_expected && !controller.inflight_nonce &&
            controller.request_count == 1 && local.ok() && utf8_equal(message_store_content(store, local.handle), utf8_literal("local")) &&
            message_composer_controller_prepare_submit(&controller, 500000, &submit).error == MessageComposerControllerError::not_found,
            "verification or restricted-action response preserves the failed message without automatic replay");
    }
    response.rejection = {};

    check(reset(), "confirmed response take failure fixture resets"); nonce = queue();
    messages[0].nonce = nonce; messages[0].fields |= gateway_message_field_nonce;
    bool confirmed = false;
    check(message_composer_controller_observe_history(&controller, {messages, 1}, &confirmed) && confirmed &&
        message_composer_controller_take_completion(&controller, &completion) &&
        message_composer_controller_submit_failed(&controller, nonce, 120).ok() && !controller.inflight_nonce,
        "response-take failure drains a previously confirmed transport receipt");
}
} // namespace maicord
