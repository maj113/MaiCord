#include "maicord/model/message_composer.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/app/message_composer_controller.hpp"
#include "maicord/app/message_send_response.hpp"
#include "maicord/app/message_history_sync.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/app/message_attachment_drafts.hpp"
#include "maicord/ui/attachment_menu.hpp"
#include "maicord/core/time.hpp"
namespace maicord {
u64 benchmark_local_attachment_roundtrip(u32 iterations, u32* peak_text, u32* record_bytes, u64* checksum, bool restore, bool history) noexcept {
    MessageRecord records[2]{}; u32 order[2]{}, heights[2]{}; u64 fenwick[2]{};
    u8 text[128]{}, draft_text[128]{}, request_text[kMessageComposerMaximumBytes]{}, rich_text[4096]{};
    char composer_text[128]{};
    MessageDraftRecord draft_records[2]{};
    MessageRichRecord rich_records[2]{}; u16 rich_order[2]{};
    MessageStore store{}; MessageRichStore rich{}; UiVariableList list{}; MessageTimeline timeline{};
    MessageComposer composer{}; MessageDraftStore drafts{}; MessageComposerController controller{};
    if (!message_store_initialize(&store, {records, 2}, {order, 2}, {text, sizeof(text)}) ||
        !ui_variable_list_initialize(&list, {heights, 2}, {fenwick, 2}, 0, 100) ||
        !message_timeline_initialize(&timeline, &store, &list, 100, 100, true) ||
        !message_rich_store_initialize(&rich, {rich_records, 2}, {rich_order, 2}, {rich_text, sizeof(rich_text)}) ||
        !message_timeline_bind_rich_store(&timeline, &rich) ||
        !message_composer_initialize(&composer, {composer_text, sizeof(composer_text)}) ||
        !message_draft_store_initialize(&drafts, {draft_records, 2}, {draft_text, sizeof(draft_text)}) ||
        !message_composer_controller_initialize(&controller, &composer, &drafts, &timeline, {request_text, sizeof(request_text)})) return 0;
    message_composer_reset(&composer, 7, false);
    const UploadedAttachment attachment{utf8_literal("report.txt"), utf8_literal("upload/report.txt"), utf8_literal("Quarterly report"), 0, false};
    const u64 size = 8192;
    *peak_text = 0; *record_bytes = sizeof(MessageRichRecord);
    const Clock clock = clock_initialize(); const i64 start = clock_now();
    for (u32 i = 0; i < iterations; ++i) {
        const u64 at = static_cast<u64>(i) * 3 + 1;
        const auto queued = message_composer_controller_queue_send(&controller, true, 9,
            1725012496900ull + i, at, 0, {&attachment, 1}, {&size, 1});
        const auto* local = queued.ok() ? message_rich_store_find(rich, queued.nonce) : nullptr;
        if (!local || local->attachment_count != 1 || local->attachments[0].size != size || local->attachments[0].url.bytes) return 0;
        if (restore) {
            bool changed = false;
            if (!message_timeline_reset(&timeline) || !message_composer_controller_restore_local(&controller, 7, &changed) || !changed) return 0;
            local = message_rich_store_find(rich, queued.nonce);
            if (!local || local->attachment_count != 1 || local->attachments[0].size != size || local->attachments[0].url.bytes) return 0;
        }
        *peak_text = maximum(*peak_text, rich.text_used);
        MessageComposerSubmit submit;
        if (!message_composer_controller_prepare_submit(&controller, at + 1, &submit).ok() ||
            !message_composer_controller_mark_submitted(&controller, submit.nonce).ok() ||
            !message_composer_controller_observe_http(&controller, 200, MessageSendHttpDisposition::success, at + 2).ok()) return 0;
        if (history) {
            GatewayMessageEvent message{};
            message.id = 1000000 + i; message.nonce = queued.nonce; message.channel_id = 7; message.author_id = 9;
            message.fields = gateway_message_field_id | gateway_message_field_channel_id |
                gateway_message_field_author_id | gateway_message_field_nonce;
            bool confirmed = false;
            if (!message_composer_controller_observe_history(&controller, {&message, 1}, &confirmed) || !confirmed) return 0;
        } else if (!message_send_tracker_observe_gateway(&controller.sends, &timeline, queued.nonce, 7, 9, 1000000 + i, at + 3).completed) return 0;
        MessageSendCompletion completion{};
        if (!message_composer_controller_take_completion(&controller, &completion) || controller.request_count || store.count || rich.count) return 0;
        *checksum += completion.nonce + completion.authoritative_id;
    }
    return clock_microseconds(clock, start, clock_now());
}

u64 benchmark_attachment_menu(u32 iterations, u64* checksum, u32* committed_bytes) noexcept {
    MessageAttachmentDraftStore drafts{};
    if (!message_attachment_drafts_initialize(&drafts)) return 0;
    const wchar_t path[] = L"C:\\report.txt";
    const MessageAttachmentFile file{{path, 13}, {{1, 2, 3, 0}, 8192}};
    u64 id = 0;
    if (message_attachment_drafts_add(&drafts, 7, {&file, 1}, {&id, 1}) != MessageAttachmentDraftError::none) {
        message_attachment_drafts_release(&drafts); return 0;
    }
    constexpr u32 count = kAttachmentFileMenuItems, widget_count = 2 + count * 2;
    PopupItem storage[count]{}, items[count]{}; PopupRowWidget rows[count]{};
    UiWidget widgets[widget_count]{}; UiLayoutNode nodes[widget_count]{}; PopupScene scene{};
    if (!popup_scene_initialize(&scene, {storage, count}, {rows, count}, {widgets, widget_count}, {nodes, widget_count})) {
        message_attachment_drafts_release(&drafts); return 0;
    }
    const auto filename = message_attachment_drafts_filename(&drafts, id);
    *committed_bytes = drafts.committed_bytes;
    const Clock clock = clock_initialize(); const i64 start = clock_now();
    bool ok = true;
    for (u32 i = 0; i < iterations; ++i) {
        const bool spoiler = (i & 1u) != 0;
        if (!message_attachment_drafts_set_spoiler(&drafts, id, spoiler) ||
            attachment_file_menu(filename, spoiler, {items, count}) != count ||
            !popup_scene_open(&scene, PopupKind::popover, 7, {items, count}, {304, 216, 305, 217}, {0, 0, 320, 240}, 360, true).ok() ||
            popup_scene_selected_action(scene) != popup_action_attachment_spoiler || drafts.committed_bytes != *committed_bytes) { ok = false; break; }
        *checksum += scene.item_count + scene.selected_index + scene.revision + items[1].flags;
        popup_scene_close(&scene);
    }
    const auto elapsed = clock_microseconds(clock, start, clock_now());
    popup_scene_release(&scene); message_attachment_drafts_release(&drafts);
    return ok ? elapsed : 0;
}

u64 benchmark_message_response_roundtrip(u32 iterations, u64* checksum) noexcept {
    MessageRecord records[2]{}; u32 order[2]{}, heights[2]{}; u64 fenwick[2]{};
    u8 text[128]{}, draft_text[128]{}, request_text[kMessageComposerMaximumBytes]{}, rich_text[4096]{};
    char composer_text[128]{}, content[512]{};
    MessageDraftRecord draft_records[2]{}; MessageRichRecord rich_records[2]{}; u16 rich_order[2]{};
    MessageStore store{}; MessageRichStore rich{}; UiVariableList list{}; MessageTimeline timeline{};
    MessageComposer composer{}; MessageDraftStore drafts{}; MessageComposerController controller{};
    if (!message_store_initialize(&store, {records, 2}, {order, 2}, {text, sizeof(text)}) ||
        !ui_variable_list_initialize(&list, {heights, 2}, {fenwick, 2}, 0, 100) ||
        !message_timeline_initialize(&timeline, &store, &list, 100, 100, true) ||
        !message_rich_store_initialize(&rich, {rich_records, 2}, {rich_order, 2}, {rich_text, sizeof(rich_text)}) ||
        !message_timeline_bind_rich_store(&timeline, &rich) ||
        !message_composer_initialize(&composer, {composer_text, sizeof(composer_text)}) ||
        !message_draft_store_initialize(&drafts, {draft_records, 2}, {draft_text, sizeof(draft_text)}) ||
        !message_composer_controller_initialize(&controller, &composer, &drafts, &timeline, {request_text, sizeof(request_text)})) return 0;
    message_composer_reset(&composer, 7, false);
    const UploadedAttachment attachment{utf8_literal("report.txt"), utf8_literal("upload/report.txt"), {}, 0, false};
    constexpr auto body = utf8_literal(R"({"id":"900","channel_id":"7","author":{"id":"9"},"content":"","timestamp":"2024-08-30T12:00:00Z","attachments":[{"id":"901","filename":"report.txt","size":8192,"url":"https://cdn.discordapp.com/attachments/7/901/report.txt"}]})");
    u32 structurals[256]{}; GatewayMessageEvent messages[1]{}; GatewayMessageAttachment attachments[1]{};
    const MessageHistoryDecodeScratch scratch{{structurals, 256}, {content, sizeof(content)}, {messages, 1}, {},
        {{}, {attachments, 1}, {}, {}, {}, {}, {}}, 32};
    RestClientResponse response{}; response.status_code = 200; response.body_bytes = body.bytes;
    const u64 size = 8192;
    const Clock clock = clock_initialize(); const i64 start = clock_now();
    for (u32 i = 0; i < iterations; ++i) {
        const u64 at = static_cast<u64>(i) * 3 + 1;
        const auto queued = message_composer_controller_queue_send(&controller, true, 9,
            1725012496900ull + i, at, 0, {&attachment, 1}, {&size, 1});
        MessageComposerSubmit submit;
        if (!queued.ok() || !message_composer_controller_prepare_submit(&controller, at + 1, &submit).ok() ||
            !message_composer_controller_mark_submitted(&controller, submit.nonce).ok()) return 0;
        const auto accepted = message_send_accept_response(&controller, response, body, scratch, 7, 9, at + 2);
        MessageSendCompletion completion{};
        const auto* authoritative = message_rich_store_find(rich, 900);
        if (!accepted.http.ok() || !accepted.body_decoded || !accepted.message.ok() || !accepted.message.changed ||
            !authoritative || authoritative->attachment_count != 1 || authoritative->attachments[0].size != size ||
            !message_composer_controller_take_completion(&controller, &completion) || !completion.response_observed ||
            completion.gateway_observed || controller.request_count || store.count != 1 || !message_timeline_reset(&timeline)) return 0;
        *checksum += completion.nonce + completion.authoritative_id;
    }
    return clock_microseconds(clock, start, clock_now());
}
}
