#include "maicord/app/message_composer_controller.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/app/message_attachment_storage.hpp"

namespace maicord {
namespace {
constexpr Utf8View kStickerPlaceholder = utf8_literal("[Sticker]");
constexpr Utf8View kForwardPlaceholder = utf8_literal("[Forwarded message]");

u32 request_storage_bytes(const MessageComposerRequest& request) noexcept {
    const u32 bytes = request.text_bytes + request.attachment_bytes;
    // Keep physical offsets distinct for empty-content sticker requests.
    return bytes ? bytes : 1;
}

// A pending voice message renders as one (waveform and duration, Ogg audio).
void local_voice(const UploadedAttachment& source, GatewayMessageAttachment* local) noexcept {
    if (!attachment_upload_is_voice(source))
        return;
    local->waveform = source.waveform;
    local->duration_ms = source.duration_ms;
    local->has_duration = true;
    local->content_type = utf8_literal("audio/ogg");
}

// Local cards contain display metadata only. Signed upload URLs and source
// paths never enter message rendering or the CDN media scheduler.
__declspec(noinline) MessageSendBeginResult begin_attachment_send(MessageComposerController* controller,
    u64 author_id, Utf8View content, u64 now_unix_ms, u64 now_monotonic_ms,
    ConstSlice<UploadedAttachment> attachments, ConstSlice<u64> sizes) noexcept {
    GatewayMessageAttachment local[kAttachmentUploadMaximumCount];
    for (u32 i = 0; i < attachments.count; ++i) {
        local[i] = {};
        local[i].id = i + 1; local[i].filename = attachments[i].filename;
        local[i].description = attachments[i].description;
        local[i].size = sizes.count ? sizes[i] : 0;
        local_voice(attachments[i], &local[i]);
    }
    return message_send_tracker_begin(&controller->sends, controller->timeline, controller->composer->channel_id,
        author_id, content, now_unix_ms, now_monotonic_ms, true, {local, attachments.count});
}

MessageComposerControllerResult controller_error(MessageComposerController* controller,
                                                 MessageComposerControllerError error) noexcept {
    saturating_increment_if(controller, controller->rejected);
    return MessageComposerControllerResult{{}, 0, error, false};
}

__declspec(noinline) bool restore_request(MessageComposerController* controller,
    const MessageComposerRequest& request, bool failed) noexcept {
    UploadedAttachment references[kAttachmentUploadMaximumCount];
    u64 sizes[kAttachmentUploadMaximumCount];
    GatewayMessageAttachment local[kAttachmentUploadMaximumCount];
    u32 count = 0;
    if (request.text_offset > controller->request_text_used ||
        request_storage_bytes(request) > controller->request_text_used - request.text_offset ||
        !request.author_id || !request.created_at_ms) return false;
    if (request.attachment_bytes && !message_attachment_storage_read(
        {controller->request_text + request.text_offset + request.text_bytes, request.attachment_bytes},
        {references, kAttachmentUploadMaximumCount}, &count, {sizes, kAttachmentUploadMaximumCount})) return false;
    for (u32 i = 0; i < count; ++i) {
        local[i] = {}; local[i].id = i + 1; local[i].filename = references[i].filename;
        local[i].description = references[i].description; local[i].size = sizes[i];
        local_voice(references[i], &local[i]);
    }
    return message_timeline_insert_local(controller->timeline,
        {request.nonce, request.channel_id, request.author_id, request.created_at_ms, 0,
            !request.text_bytes && request.forward_reference.message_id ? kForwardPlaceholder :
            !request.text_bytes && request.sticker_count ? kStickerPlaceholder :
                Utf8View{reinterpret_cast<const char*>(controller->request_text + request.text_offset), request.text_bytes}, 0,
            MessageContentAvailability::available, failed ? MessageDeliveryState::failed : MessageDeliveryState::pending},
        controller->timeline->rich_store ? ConstSlice<GatewayMessageAttachment>{local, count} : ConstSlice<GatewayMessageAttachment>{}).ok();
}

bool controller_valid(const MessageComposerController* controller) noexcept {
    return controller && controller->initialized && controller->composer && controller->composer->text &&
           controller->drafts && controller->timeline && controller->timeline->store &&
           controller->request_text && controller->request_text_capacity != 0 &&
           controller->request_text_used <= controller->request_text_capacity &&
           controller->request_text_wasted <= controller->request_text_used &&
           controller->request_count <= kMessageComposerRequestCapacity;
}

MessageComposerRequest* find_request(MessageComposerController* controller, u64 nonce) noexcept {
    if (nonce == 0) {
        return nullptr;
    }
    for (u32 index = 0; index < kMessageComposerRequestCapacity; ++index) {
        MessageComposerRequest& request = controller->requests[index];
        if (request.occupied && request.nonce == nonce) {
            return &request;
        }
    }
    return nullptr;
}

bool delivery_completed(const MessageComposerController& controller, const MessageComposerRequest* request, u64 nonce) noexcept {
    if (!request || request->phase == MessageComposerRequestPhase::confirmed) return true;
    const auto& sends = controller.sends;
    for (u32 i = 0; i < sends.completion_count; ++i) {
        const auto& completion = sends.completions[(sends.completion_read + i) % kMessageSendTrackerCapacity];
        if (completion.nonce == nonce && completion.kind == MessageSendCompletionKind::accepted) return true;
    }
    return false;
}

MessageComposerRequest* empty_request(MessageComposerController* controller) noexcept {
    for (u32 index = 0; index < kMessageComposerRequestCapacity; ++index) {
        if (!controller->requests[index].occupied) {
            return &controller->requests[index];
        }
    }
    return nullptr;
}

bool compact_requests(MessageComposerController* controller) noexcept {
    if (!controller_valid(controller)) {
        return false;
    }
    u32 write_offset = 0;
    u32 previous_source = 0;
    bool has_previous = false;
    for (u32 processed = 0; processed < controller->request_count; ++processed) {
        MessageComposerRequest* selected = nullptr;
        u32 selected_source = ~0u;
        for (u32 index = 0; index < kMessageComposerRequestCapacity; ++index) {
            MessageComposerRequest& request = controller->requests[index];
            if (!request.occupied || (has_previous && request.text_offset <= previous_source) ||
                request.text_offset >= selected_source) {
                continue;
            }
            selected = &request;
            selected_source = request.text_offset;
        }
        if (!selected || selected->text_offset > controller->request_text_used ||
            request_storage_bytes(*selected) > controller->request_text_used - selected->text_offset) {
            return false;
        }
        copy_bytes(controller->request_text + write_offset,
                   controller->request_text + selected->text_offset, request_storage_bytes(*selected));
        selected->text_offset = write_offset;
        write_offset += request_storage_bytes(*selected);
        previous_source = selected_source;
        has_previous = true;
    }
    clear_bytes(controller->request_text + write_offset, controller->request_text_used - write_offset);
    controller->request_text_used = write_offset;
    controller->request_text_wasted = 0;
    ++controller->compactions;
    return true;
}

void erase_request(MessageComposerController* controller, MessageComposerRequest* request) noexcept {
    clear_bytes(controller->request_text + request->text_offset, request_storage_bytes(*request));
    controller->request_text_wasted += request_storage_bytes(*request);
    *request = MessageComposerRequest{};
    if (controller->request_count != 0) {
        --controller->request_count;
    }
    if (controller->request_count == 0) {
        clear_bytes(controller->request_text, controller->request_text_used);
        controller->request_text_used = 0;
        controller->request_text_wasted = 0;
    }
}

MessageComposerControllerError tracker_error(MessageSendTrackerError error) noexcept {
    return error == MessageSendTrackerError::none ? MessageComposerControllerError::none
                                                  : MessageComposerControllerError::tracker;
}

void advance_reply_revision(MessageComposerController* controller) noexcept {
    if (!++controller->reply_revision) ++controller->reply_revision;
}

bool reply_snapshot_matches(const MessageComposerController& controller,
                            const MessageComposerReplySnapshot& snapshot) noexcept {
    return snapshot.channel_id == controller.composer->channel_id &&
        snapshot.message_id == controller.reply_message_id &&
        snapshot.revision == controller.reply_revision &&
        snapshot.mention_reply_author == controller.mention_reply_author;
}

MessageComposerControllerResult save_active_draft(MessageComposerController* controller) noexcept {
    MessageComposer& composer = *controller->composer;
    if (composer.channel_id == 0) {
        return MessageComposerControllerResult{};
    }
    const MessageDraftStoreResult saved = message_draft_store_save(
        controller->drafts, MessageDraftInput{composer.channel_id, message_composer_text(composer),
            composer.anchor, composer.caret, controller->reply_message_id, controller->mention_reply_author});
    if (!saved.ok()) {
        return controller_error(controller, MessageComposerControllerError::draft);
    }
    if (saved.changed) {
        ++controller->draft_commits;
    }
    return MessageComposerControllerResult{{}, 0, MessageComposerControllerError::none, saved.changed};
}

} // namespace

namespace {
bool initialize_controller(MessageComposerController* controller, MessageComposer* composer,
                           MessageDraftStore* drafts, MessageTimeline* timeline, Slice<u8> request_text,
                           bool clear_requests) noexcept;
} // namespace

bool message_composer_controller_initialize(MessageComposerController* controller,
                                            MessageComposer* composer, MessageDraftStore* drafts,
                                            MessageTimeline* timeline, Slice<u8> request_text) noexcept {
    return initialize_controller(controller, composer, drafts, timeline, request_text, true);
}

bool message_composer_controller_initialize_sparse(MessageComposerController* controller,
                                                   MessageComposer* composer, MessageDraftStore* drafts,
                                                   MessageTimeline* timeline, Slice<u8> request_text) noexcept {
    return initialize_controller(controller, composer, drafts, timeline, request_text, false);
}

namespace {
bool initialize_controller(MessageComposerController* controller, MessageComposer* composer,
                           MessageDraftStore* drafts, MessageTimeline* timeline, Slice<u8> request_text,
                           bool clear_requests) noexcept {
    if (!controller || !composer || !composer->text || !drafts || !drafts->records || !drafts->text ||
        !timeline || !timeline->store || !request_text.data ||
        request_text.count < kMessageComposerMaximumBytes) {
        return false;
    }
    *controller = MessageComposerController{};
    controller->mention_reply_author = true;
    controller->composer = composer;
    controller->drafts = drafts;
    controller->timeline = timeline;
    controller->request_text = request_text.data;
    controller->request_text_capacity = request_text.count;
    if (clear_requests)
        clear_bytes(request_text.data, request_text.count);
    if (!message_send_tracker_initialize(&controller->sends)) {
        *controller = MessageComposerController{};
        return false;
    }
    controller->initialized = true;
    controller->reply_revision = 1;
    return true;
}
} // namespace

MessageComposerControllerResult message_composer_controller_commit_draft(
    MessageComposerController* controller) noexcept {
    return controller_valid(controller)
               ? save_active_draft(controller)
               : controller_error(controller, MessageComposerControllerError::invalid_input);
}

MessageComposerControllerResult message_composer_controller_select_channel(
    MessageComposerController* controller, u64 channel_id) noexcept {
    if (!controller_valid(controller)) {
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    }
    if (controller->composer->channel_id == channel_id) {
        return MessageComposerControllerResult{};
    }
    const MessageComposerControllerResult saved = save_active_draft(controller);
    if (!saved.ok()) {
        return saved;
    }
    if (channel_id == 0) {
        message_composer_reset(controller->composer, 0, false);
        controller->reply_message_id = 0;
        controller->mention_reply_author = true;
    } else {
        MessageDraftSnapshot draft{};
        if (message_draft_store_snapshot(*controller->drafts, channel_id, &draft)) {
            if (!message_composer_restore(controller->composer, channel_id, draft.text, draft.anchor,
                                          draft.caret)) {
                return controller_error(controller, MessageComposerControllerError::draft);
            }
            controller->reply_message_id = draft.reply_message_id;
            controller->mention_reply_author = !draft.reply_message_id || draft.mention_reply_author;
        } else if (message_draft_store_find(*controller->drafts, channel_id).error !=
                   MessageDraftStoreError::not_found) {
            return controller_error(controller, MessageComposerControllerError::draft);
        } else {
            message_composer_reset(controller->composer, channel_id, false);
            controller->reply_message_id = 0;
            controller->mention_reply_author = true;
        }
    }
    ++controller->channel_switches;
    advance_reply_revision(controller);
    return MessageComposerControllerResult{{}, 0, MessageComposerControllerError::none, true};
}

MessageComposerControllerResult message_composer_controller_set_reply(
    MessageComposerController* controller, u64 reply_message_id, bool mention_reply_author) noexcept {
    if (!controller_valid(controller) || !controller->composer->channel_id)
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    mention_reply_author = !reply_message_id || mention_reply_author;
    const auto& composer = *controller->composer;
    const auto saved = message_draft_store_save(controller->drafts,
        {composer.channel_id, message_composer_text(composer), composer.anchor, composer.caret,
            reply_message_id, mention_reply_author});
    if (!saved.ok()) return controller_error(controller, MessageComposerControllerError::draft);
    const bool changed = controller->reply_message_id != reply_message_id ||
        controller->mention_reply_author != mention_reply_author;
    controller->reply_message_id = reply_message_id;
    controller->mention_reply_author = mention_reply_author;
    if (changed) advance_reply_revision(controller);
    if (saved.changed) ++controller->draft_commits;
    return {{}, 0, MessageComposerControllerError::none, changed || saved.changed};
}

bool message_composer_controller_reply_snapshot(const MessageComposerController& controller,
    MessageComposerReplySnapshot* snapshot) noexcept {
    if (snapshot) *snapshot = {};
    if (!snapshot || !controller_valid(&controller) || !controller.composer->channel_id) return false;
    *snapshot = {controller.composer->channel_id, controller.reply_message_id,
        controller.reply_revision, controller.mention_reply_author};
    return true;
}

MessageComposerControllerResult message_composer_controller_clear_reply(
    MessageComposerController* controller, u64 channel_id, u64 reply_message_id) noexcept {
    if (!controller_valid(controller) || !channel_id || !reply_message_id)
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    const auto cleared = message_draft_store_clear_reply(controller->drafts, channel_id, reply_message_id);
    if (!cleared.ok()) return controller_error(controller, MessageComposerControllerError::draft);
    bool changed = cleared.changed;
    if (controller->composer->channel_id == channel_id && controller->reply_message_id == reply_message_id) {
        controller->reply_message_id = 0;
        controller->mention_reply_author = true;
        advance_reply_revision(controller);
        changed = true;
    }
    for (auto& request : controller->requests) {
        if (!request.occupied || request.channel_id != channel_id || request.reply_message_id != reply_message_id ||
            request.reply_unavailable || request.phase == MessageComposerRequestPhase::confirmed) continue;
        request.reply_unavailable = true;
        changed = true;
    }
    if (cleared.changed) ++controller->draft_commits;
    return {{}, 0, MessageComposerControllerError::none, changed};
}

namespace {
// detached: a separate source with its own explicit reply target; the active
// draft's reply is neither required nor consumed. silent forces
// SUPPRESS_NOTIFICATIONS.
MessageComposerControllerResult queue_send(
    MessageComposerController* controller, bool channel_allows_send, u64 author_id, u64 now_unix_ms,
    u64 now_monotonic_ms, u64 reply_message_id, ConstSlice<UploadedAttachment> attachments,
    ConstSlice<u64> attachment_sizes, MessageComposer* source, ConstSlice<u64> sticker_ids,
    bool mention_reply_author, const MessageComposerReplySnapshot* reply_snapshot, bool detached,
    bool silent) noexcept {
    auto* composer = source ? source : controller ? controller->composer : nullptr;
    if (!controller_valid(controller) || !composer || (source && attachments.count) || author_id == 0 || now_unix_ms == 0 || now_monotonic_ms == 0 ||
        (attachment_sizes.count && (!attachment_sizes.data || attachment_sizes.count != attachments.count)) ||
        sticker_ids.count > kMessageCreateMaximumStickerCount || (!sticker_ids.data && sticker_ids.count) ||
        !message_composer_can_send(*composer, channel_allows_send,
            kMessageComposerDefaultMaximumCodepoints, attachments.count != 0 || sticker_ids.count != 0)) {
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    }
    const bool active_source = composer == controller->composer;
    if (detached && (active_source || reply_snapshot))
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    // The active draft is the source of truth. A separate producer must capture it
    // first before it can consume a reply choice.
    if (!detached && ((active_source && (reply_message_id != controller->reply_message_id ||
            (reply_message_id && mention_reply_author != controller->mention_reply_author))) ||
        (reply_snapshot && (!reply_snapshot_matches(*controller, *reply_snapshot) ||
            reply_message_id != reply_snapshot->message_id ||
            (reply_message_id && mention_reply_author != reply_snapshot->mention_reply_author))) ||
        (!active_source && reply_message_id && (!reply_snapshot || composer->channel_id != reply_snapshot->channel_id ||
            reply_message_id != reply_snapshot->message_id || mention_reply_author != reply_snapshot->mention_reply_author))))
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    u64 retained_stickers[kMessageCreateMaximumStickerCount]{};
    for (u32 i = 0; i < sticker_ids.count; ++i) {
        if (!sticker_ids[i]) return controller_error(controller, MessageComposerControllerError::invalid_input);
        for (u32 j = 0; j < i; ++j)
            if (sticker_ids[i] == retained_stickers[j])
                return controller_error(controller, MessageComposerControllerError::invalid_input);
        retained_stickers[i] = sticker_ids[i];
    }
    const u32 attachment_bytes = attachments.count ? message_attachment_storage_bytes(attachments, attachment_sizes) : 0;
    if (attachments.count && !attachment_bytes)
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    // prepare_submit views borrow this bank; reject them as new send inputs
    // before compaction can invalidate a source pointer.
    const u64 bank_begin = reinterpret_cast<u64>(controller->request_text);
    const u64 bank_end = bank_begin + controller->request_text_capacity;
    const u64 sizes_begin = reinterpret_cast<u64>(attachment_sizes.data);
    if (attachment_sizes.count && sizes_begin < bank_end && sizes_begin + attachment_sizes.count * sizeof(u64) > bank_begin)
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    for (u32 i = 0; i < attachments.count; ++i) {
        const Utf8View strings[]{attachments[i].filename, attachments[i].uploaded_filename, attachments[i].description,
                                 attachments[i].waveform};
        for (const auto text : strings) {
            const u64 begin = reinterpret_cast<u64>(text.data);
            if (text.bytes && begin < bank_end && begin + text.bytes > bank_begin)
                return controller_error(controller, MessageComposerControllerError::invalid_input);
        }
    }
    MessageComposerRequest* const request = empty_request(controller);
    Utf8View content = message_composer_text(*composer);
    // A leading "@silent" is stripped and sent as SUPPRESS_NOTIFICATIONS.
    const bool suppress_notifications = message_silent_prefix(content, &content) || silent;
    if (suppress_notifications && !content.bytes && !attachments.count && !sticker_ids.count)
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    const u64 content_begin = reinterpret_cast<u64>(content.data);
    if (content.bytes && content_begin < bank_end && content_begin + content.bytes > bank_begin)
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    if (!request) {
        return controller_error(controller, MessageComposerControllerError::request_capacity);
    }
    const u32 live_bytes = controller->request_text_used - controller->request_text_wasted;
    const u32 payload_bytes = content.bytes + attachment_bytes;
    const u32 storage_bytes = payload_bytes ? payload_bytes : 1;
    if (storage_bytes > controller->request_text_capacity - live_bytes) {
        return controller_error(controller, MessageComposerControllerError::text_capacity);
    }
    if (storage_bytes > controller->request_text_capacity - controller->request_text_used &&
        !compact_requests(controller)) {
        return controller_error(controller, MessageComposerControllerError::text_capacity);
    }
    const Utf8View display_content = !content.bytes && sticker_ids.count ? kStickerPlaceholder : content;
    if (!active_source && !detached && reply_message_id) {
        const auto saved = save_active_draft(controller);
        if (!saved.ok()) return saved;
    }
    const MessageSendBeginResult begun = attachments.count && controller->timeline->rich_store ?
        begin_attachment_send(controller, author_id, display_content, now_unix_ms, now_monotonic_ms, attachments, attachment_sizes) : message_send_tracker_begin(
        &controller->sends, controller->timeline, composer->channel_id, author_id, display_content,
        now_unix_ms, now_monotonic_ms, attachments.count != 0);
    if (!begun.ok()) {
        return controller_error(controller, tracker_error(begun.error));
    }
    ++controller->request_sequence;
    if (controller->request_sequence == 0) {
        ++controller->request_sequence;
    }
    *request = MessageComposerRequest{
        begun.nonce,
        composer->channel_id,
        reply_message_id,
        0,
        controller->request_text_used,
        content.bytes,
        controller->request_sequence,
        0,
        MessageComposerRequestPhase::queued,
        true,
        attachment_bytes,
        author_id,
        now_unix_ms,
    };
    request->sticker_count = sticker_ids.count;
    request->mention_reply_author = reply_message_id != 0 && mention_reply_author;
    request->suppress_notifications = suppress_notifications;
    for (u32 i = 0; i < sticker_ids.count; ++i) request->sticker_ids[i] = retained_stickers[i];
    copy_bytes(controller->request_text + controller->request_text_used,
               reinterpret_cast<const u8*>(content.data), content.bytes);
    if (!payload_bytes) controller->request_text[controller->request_text_used] = 0;
    if (attachment_bytes) {
        // Already validated and capacity-checked before the tracker commit.
        message_attachment_storage_write(attachments,
            {controller->request_text + controller->request_text_used + content.bytes, attachment_bytes}, attachment_sizes);
    }
    controller->request_text_used += storage_bytes;
    ++controller->request_count;
    ++controller->queued;
    const u64 channel_id = composer->channel_id;
    const bool focused = composer->focused;
    message_composer_clear(composer);
    composer->channel_id = channel_id;
    composer->focused = focused;
    // The request now lives in the request bank and tracker. The validated
    // store makes this deletion idempotent, so it must not report a send
    // failure after the send was committed.
    if (active_source) {
        message_draft_store_delete(controller->drafts, channel_id);
        controller->reply_message_id = 0;
        controller->mention_reply_author = true;
        advance_reply_revision(controller);
    } else if (reply_message_id && !detached) {
        message_draft_store_clear_reply(controller->drafts, channel_id, reply_message_id);
        controller->reply_message_id = 0;
        controller->mention_reply_author = true;
        advance_reply_revision(controller);
    }
    return MessageComposerControllerResult{begun.invalidation, begun.nonce,
                                           MessageComposerControllerError::none, true};
}
} // namespace

MessageComposerControllerResult message_composer_controller_queue_send(
    MessageComposerController* controller, bool channel_allows_send, u64 author_id, u64 now_unix_ms,
    u64 now_monotonic_ms, u64 reply_message_id, ConstSlice<UploadedAttachment> attachments,
    ConstSlice<u64> attachment_sizes, MessageComposer* source, ConstSlice<u64> sticker_ids,
    bool mention_reply_author, const MessageComposerReplySnapshot* reply_snapshot) noexcept {
    return queue_send(controller, channel_allows_send, author_id, now_unix_ms, now_monotonic_ms, reply_message_id,
                      attachments, attachment_sizes, source, sticker_ids, mention_reply_author, reply_snapshot, false,
                      false);
}

MessageComposerControllerResult message_composer_controller_queue_text(
    MessageComposerController* controller, MessageComposer* source, u64 author_id, u64 now_unix_ms,
    u64 now_monotonic_ms, u64 reply_message_id, bool mention_reply_author, bool silent) noexcept {
    if (!source || (controller && source == controller->composer))
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    return queue_send(controller, true, author_id, now_unix_ms, now_monotonic_ms, reply_message_id, {}, {}, source,
                      {}, reply_message_id != 0 && mention_reply_author, nullptr, true, silent);
}

namespace {
// 0 none, 1 @everyone, 2 @here: the first mention outside code spans.
u32 first_global_mention(Utf8View content) noexcept {
    constexpr Utf8View everyone = utf8_literal("@everyone"), here = utf8_literal("@here");
    for (u32 index = 0; content.data && index < content.bytes;) {
        const char value = content.data[index];
        if (value == '\\') {
            index += 2;
            continue;
        }
        if (value == '`') {
            u32 run = 0;
            while (index + run < content.bytes && content.data[index + run] == '`')
                ++run;
            // A code span closes on the next run of the same length.
            u32 close = index + run;
            bool closed = false;
            while (close < content.bytes) {
                if (content.data[close] != '`') {
                    ++close;
                    continue;
                }
                u32 length = 0;
                while (close + length < content.bytes && content.data[close + length] == '`')
                    ++length;
                if (length == run) {
                    closed = true;
                    break;
                }
                close += length;
            }
            index = closed ? close + run : index + run;
            continue;
        }
        if (value == '@') {
            const u32 remaining = content.bytes - index;
            if (remaining >= everyone.bytes && bytes_equal(content.data + index, everyone.data, everyone.bytes))
                return 1;
            if (remaining >= here.bytes && bytes_equal(content.data + index, here.data, here.bytes))
                return 2;
        }
        ++index;
    }
    return 0;
}

u32 append_text(Slice<char> output, u32 used, Utf8View text) noexcept {
    if (used > output.count || text.bytes > output.count - used)
        return ~0u;
    copy_bytes(output.data + used, text.data, text.bytes);
    return used + text.bytes;
}
} // namespace

u32 message_mention_warning_round(u32 count) noexcept {
    u32 scale = 1;
    while (count / scale >= 10)
        scale *= 10;
    return count / scale * scale;
}

MessageMentionWarning message_mention_warning_evaluate(const MessageMentionWarningInput& input) noexcept {
    MessageMentionWarning warning{};
    if (!input.guild_channel || !input.can_mention_everyone)
        return warning;
    const u32 mention = first_global_mention(input.content);
    if (!mention)
        return warning;
    const u32 people = input.place != MessageMentionWarningPlace::channel ? input.thread_member_count
                       : mention == 1                                    ? input.member_count
                                                                         : input.online_count;
    if (people <= kMessageMentionWarningThreshold)
        return warning;
    warning.required = true;
    warning.here = mention == 2;
    warning.count = message_mention_warning_round(people);
    return warning;
}

u32 message_mention_warning_body(const MessageMentionWarning& warning, MessageMentionWarningPlace place,
                                 Slice<char> output) noexcept {
    if (!warning.required || !output.data)
        return 0;
    char digits[16]{};
    const u32 digit_count = decimal_write_grouped(digits, warning.count);
    u32 used = append_text(output, 0, utf8_literal("By using "));
    used = append_text(output, used, warning.here ? utf8_literal("@here") : utf8_literal("@everyone"));
    used = append_text(output, used, utf8_literal(", you are about to mention "));
    used = append_text(output, used, {digits, digit_count});
    used = append_text(output, used,
                       place == MessageMentionWarningPlace::thread       ? utf8_literal("+ people in this thread.")
                       : place == MessageMentionWarningPlace::forum_post ? utf8_literal("+ people in this post.")
                                                                         : utf8_literal("+ people."));
    used = append_text(output, used, utf8_literal(" Are you sure?"));
    return used == ~0u ? 0 : used;
}

MessageComposerControllerResult message_composer_controller_queue_forward(
    MessageComposerController* controller, u64 destination_channel_id, Utf8View caption,
    MessageForwardReference source, u64 author_id, u64 now_unix_ms,
    u64 now_monotonic_ms) noexcept {
    if (!controller_valid(controller) || !destination_channel_id || !author_id || !now_unix_ms ||
        !now_monotonic_ms || !message_forward_reference_valid(source) ||
        (!caption.data && caption.bytes) || caption.bytes > kMessageComposerMaximumBytes)
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    const auto validation = utf8_validate(caption);
    if (!validation.valid || validation.codepoints > kMessageComposerDefaultMaximumCodepoints)
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    // Compaction must not invalidate a caption borrowed from an older submit.
    const u64 caption_begin = reinterpret_cast<u64>(caption.data);
    const u64 bank_begin = reinterpret_cast<u64>(controller->request_text);
    if (caption.bytes && caption_begin < bank_begin + controller->request_text_capacity &&
        caption_begin + caption.bytes > bank_begin)
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    auto* request = empty_request(controller);
    if (!request) return controller_error(controller, MessageComposerControllerError::request_capacity);
    const u32 storage_bytes = caption.bytes ? caption.bytes : 1;
    const u32 live_bytes = controller->request_text_used - controller->request_text_wasted;
    if (storage_bytes > controller->request_text_capacity - live_bytes ||
        (storage_bytes > controller->request_text_capacity - controller->request_text_used && !compact_requests(controller)))
        return controller_error(controller, MessageComposerControllerError::text_capacity);
    const auto begun = message_send_tracker_begin(&controller->sends, controller->timeline,
        destination_channel_id, author_id, caption.bytes ? caption : kForwardPlaceholder,
        now_unix_ms, now_monotonic_ms, false, {}, false);
    if (!begun.ok()) return controller_error(controller, tracker_error(begun.error));
    if (!++controller->request_sequence) ++controller->request_sequence;
    *request = {};
    request->nonce = begun.nonce;
    request->channel_id = destination_channel_id;
    request->text_offset = controller->request_text_used;
    request->text_bytes = caption.bytes;
    request->sequence = controller->request_sequence;
    request->phase = MessageComposerRequestPhase::queued;
    request->occupied = true;
    request->author_id = author_id;
    request->created_at_ms = now_unix_ms;
    request->forward_reference = source;
    copy_bytes(controller->request_text + request->text_offset, caption.data, caption.bytes);
    if (!caption.bytes) controller->request_text[request->text_offset] = 0;
    controller->request_text_used += storage_bytes;
    ++controller->request_count;
    ++controller->queued;
    return {{}, begun.nonce, MessageComposerControllerError::none, true};
}

MessageComposerControllerResult message_composer_controller_cancel_unsent(
    MessageComposerController* controller, u64 nonce) noexcept {
    if (!controller_valid(controller) || !nonce)
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    auto* request = find_request(controller, nonce);
    if (!request || (request->phase != MessageComposerRequestPhase::queued &&
        request->phase != MessageComposerRequestPhase::retry_wait))
        return controller_error(controller, MessageComposerControllerError::not_found);
    if (controller->inflight_nonce == nonce)
        return controller_error(controller, MessageComposerControllerError::busy);
    const auto cancelled = message_send_tracker_cancel_unsent(&controller->sends, controller->timeline, nonce);
    if (!cancelled.ok()) return controller_error(controller, tracker_error(cancelled.error));
    erase_request(controller, request);
    return {cancelled.invalidation, nonce, MessageComposerControllerError::none, true};
}

MessageComposerControllerResult message_composer_controller_prepare_submit(
    MessageComposerController* controller, u64 now_monotonic_ms, MessageComposerSubmit* submit) noexcept {
    if (submit) {
        submit->content = {};
        submit->nonce = submit->channel_id = submit->reply_message_id = 0;
        submit->attachment_count = 0;
        submit->sticker_count = 0;
        submit->mention_reply_author = false;
        submit->suppress_notifications = false;
        submit->forward_reference = {};
    }
    if (!controller_valid(controller) || !submit || now_monotonic_ms == 0) {
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    }
    if (controller->inflight_nonce != 0) {
        return MessageComposerControllerResult{
            {}, controller->inflight_nonce, MessageComposerControllerError::busy, false};
    }
    MessageComposerRequest* selected = nullptr;
    for (u32 index = 0; index < kMessageComposerRequestCapacity; ++index) {
        MessageComposerRequest& request = controller->requests[index];
        const bool ready =
            request.occupied && (request.phase == MessageComposerRequestPhase::queued ||
                                 (request.phase == MessageComposerRequestPhase::retry_wait &&
                                  request.retry_at_ms != 0 && now_monotonic_ms >= request.retry_at_ms));
        if (ready && (!selected || request.sequence < selected->sequence)) {
            selected = &request;
        }
    }
    if (!selected) {
        return MessageComposerControllerResult{{}, 0, MessageComposerControllerError::not_found, false};
    }
    if (selected->reply_unavailable) {
        const auto failed = message_composer_controller_submit_failed(controller, selected->nonce, now_monotonic_ms);
        if (!failed.ok()) return failed;
        return {failed.invalidation, selected->nonce, MessageComposerControllerError::reply_unavailable, failed.changed};
    }
    if (selected->text_offset > controller->request_text_used ||
        request_storage_bytes(*selected) > controller->request_text_used - selected->text_offset ||
        selected->sticker_count > kMessageCreateMaximumStickerCount) {
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    }
    submit->content = {reinterpret_cast<const char*>(controller->request_text + selected->text_offset), selected->text_bytes};
    submit->nonce = selected->nonce;
    submit->channel_id = selected->channel_id;
    submit->reply_message_id = selected->reply_message_id;
    submit->forward_reference = selected->forward_reference;
    submit->mention_reply_author = selected->reply_message_id != 0 && selected->mention_reply_author;
    submit->suppress_notifications = selected->suppress_notifications;
    submit->sticker_count = selected->sticker_count;
    for (u32 i = 0; i < selected->sticker_count; ++i) submit->sticker_ids[i] = selected->sticker_ids[i];
    if (selected->attachment_bytes && !message_attachment_storage_read(
        {controller->request_text + selected->text_offset + selected->text_bytes, selected->attachment_bytes},
        {submit->attachments, kAttachmentUploadMaximumCount}, &submit->attachment_count)) {
        submit->content = {};
        submit->nonce = submit->channel_id = submit->reply_message_id = 0;
        submit->attachment_count = 0;
        submit->sticker_count = 0;
        submit->mention_reply_author = false;
        submit->suppress_notifications = false;
        submit->forward_reference = {};
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    }
    return MessageComposerControllerResult{
        {}, selected->nonce, MessageComposerControllerError::none, false};
}

MessageComposerControllerResult message_composer_controller_mark_submitted(
    MessageComposerController* controller, u64 nonce) noexcept {
    if (!controller_valid(controller) || nonce == 0) {
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    }
    if (controller->inflight_nonce != 0) {
        return controller_error(controller, MessageComposerControllerError::busy);
    }
    MessageComposerRequest* const request = find_request(controller, nonce);
    if (!request || (request->phase != MessageComposerRequestPhase::queued &&
                     request->phase != MessageComposerRequestPhase::retry_wait)) {
        return controller_error(controller, MessageComposerControllerError::not_found);
    }
    if (request->reply_unavailable)
        return controller_error(controller, MessageComposerControllerError::reply_unavailable);
    request->phase = MessageComposerRequestPhase::submitting;
    request->retry_at_ms = 0;
    controller->inflight_nonce = nonce;
    ++controller->submitted;
    return MessageComposerControllerResult{{}, nonce, MessageComposerControllerError::none, true};
}

MessageComposerControllerResult message_composer_controller_submit_failed(
    MessageComposerController* controller, u64 nonce, u64 now_monotonic_ms) noexcept {
    if (!controller_valid(controller) || nonce == 0 || now_monotonic_ms == 0) {
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    }
    MessageComposerRequest* const request = find_request(controller, nonce);
    if (controller->inflight_nonce == nonce && delivery_completed(*controller, request, nonce)) {
        controller->inflight_nonce = 0;
        return MessageComposerControllerResult{{}, nonce, MessageComposerControllerError::none, false};
    }
    if (!request) {
        return controller_error(controller, MessageComposerControllerError::not_found);
    }
    const MessageSendObserveResult observed =
        message_send_tracker_observe_http(&controller->sends, controller->timeline, nonce, 0,
                                          MessageSendHttpDisposition::terminal_failure, now_monotonic_ms);
    if (!observed.ok()) {
        return controller_error(controller, tracker_error(observed.error));
    }
    request->phase = MessageComposerRequestPhase::failed;
    request->http_status = 0;
    request->retry_at_ms = 0;
    if (controller->inflight_nonce == nonce) {
        controller->inflight_nonce = 0;
    }
    ++controller->failed;
    return MessageComposerControllerResult{observed.invalidation, nonce,
                                           MessageComposerControllerError::none, observed.changed};
}

MessageComposerControllerResult message_composer_controller_observe_http(
    MessageComposerController* controller, u16 http_status, MessageSendHttpDisposition disposition,
    u64 now_monotonic_ms, u64 retry_at_ms) noexcept {
    if (!controller_valid(controller) || controller->inflight_nonce == 0 || now_monotonic_ms == 0) {
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    }
    const u64 nonce = controller->inflight_nonce;
    MessageComposerRequest* const request = find_request(controller, nonce);
    // Delivery may already have completed through history, or through a
    // Gateway observation followed by the tracker deadline. Consume the old
    // transport response without assigning it to a subsequent send.
    if (delivery_completed(*controller, request, nonce)) {
        controller->inflight_nonce = 0;
        return MessageComposerControllerResult{{}, nonce, MessageComposerControllerError::none, false};
    }
    if (request->phase != MessageComposerRequestPhase::submitting &&
        request->phase != MessageComposerRequestPhase::failed) {
        return controller_error(controller, MessageComposerControllerError::not_found);
    }
    const MessageSendObserveResult observed =
        message_send_tracker_observe_http(&controller->sends, controller->timeline, nonce, http_status,
                                          disposition, now_monotonic_ms, retry_at_ms);
    if (!observed.ok()) {
        return controller_error(controller, tracker_error(observed.error));
    }
    request->http_status = http_status;
    controller->inflight_nonce = 0;
    if (disposition == MessageSendHttpDisposition::retry_wait) {
        request->phase = MessageComposerRequestPhase::retry_wait;
        request->retry_at_ms = retry_at_ms;
    } else if (disposition == MessageSendHttpDisposition::success) {
        request->phase = MessageComposerRequestPhase::awaiting_gateway;
        request->retry_at_ms = 0;
    } else {
        request->phase = MessageComposerRequestPhase::failed;
        request->retry_at_ms = 0;
        ++controller->failed;
    }
    return MessageComposerControllerResult{observed.invalidation, nonce,
                                           MessageComposerControllerError::none, observed.changed};
}

MessageComposerControllerResult message_composer_controller_retry(MessageComposerController* controller,
                                                                  u64 nonce,
                                                                  u64 now_monotonic_ms) noexcept {
    if (!controller_valid(controller) || nonce == 0 || now_monotonic_ms == 0) {
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    }
    MessageComposerRequest* const request = find_request(controller, nonce);
    if (!request || request->phase != MessageComposerRequestPhase::failed) {
        return controller_error(controller, MessageComposerControllerError::not_found);
    }
    if (controller->inflight_nonce == nonce) {
        return controller_error(controller, MessageComposerControllerError::busy);
    }
    if (request->reply_unavailable)
        return controller_error(controller, MessageComposerControllerError::reply_unavailable);
    const MessageSendObserveResult retried =
        message_send_tracker_retry(&controller->sends, controller->timeline, nonce, now_monotonic_ms);
    if (!retried.ok()) {
        return controller_error(controller, tracker_error(retried.error));
    }
    request->phase = MessageComposerRequestPhase::queued;
    request->http_status = 0;
    request->retry_at_ms = 0;
    return MessageComposerControllerResult{retried.invalidation, nonce,
                                           MessageComposerControllerError::none, true};
}

MessageComposerControllerResult message_composer_controller_discard_failed(
    MessageComposerController* controller, u64 nonce) noexcept {
    if (!controller_valid(controller) || nonce == 0) {
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    }
    MessageComposerRequest* const request = find_request(controller, nonce);
    if (!request || request->phase != MessageComposerRequestPhase::failed ||
        controller->inflight_nonce == nonce) {
        return controller_error(controller, MessageComposerControllerError::not_found);
    }
    const MessageSendObserveResult discarded =
        message_send_tracker_discard(&controller->sends, controller->timeline, nonce);
    if (!discarded.ok()) {
        return controller_error(controller, tracker_error(discarded.error));
    }
    erase_request(controller, request);
    return MessageComposerControllerResult{discarded.invalidation, nonce,
                                           MessageComposerControllerError::none, true};
}

MessageComposerControllerResult message_composer_controller_observe_response(MessageComposerController* controller,
    u64 nonce, const GatewayMessageEvent& message, u64 active_channel_id) noexcept {
    if (!controller_valid(controller) || !nonce)
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    auto* request = find_request(controller, nonce);
    const MessageSendAttempt* attempt = nullptr;
    for (const auto& candidate : controller->sends.attempts)
        if (candidate.active && candidate.nonce == nonce) { attempt = &candidate; break; }
    // Gateway completion, including a subsequent edit/delete, wins over an
    // older POST response. Do not recreate a message from that stale body.
    if (!request || !attempt || request->phase == MessageComposerRequestPhase::confirmed)
        return MessageComposerControllerResult{{}, nonce, MessageComposerControllerError::none, false};
    if (!attempt->http_observed || !attempt->http_success ||
        request->phase != MessageComposerRequestPhase::awaiting_gateway || !message.id ||
        message.channel_id != request->channel_id || message.author_id != request->author_id ||
        (message.nonce && message.nonce != nonce) ||
        !message_reconcile_validate_create(controller->timeline->store, message,
            MessageContentAvailability::available).ok())
        return controller_error(controller, MessageComposerControllerError::invalid_input);
    MessageTimelineInvalidation invalidation{};
    if (active_channel_id == message.channel_id) {
        const auto applied = message_timeline_apply_created(controller->timeline, message);
        if (!applied.ok()) return controller_error(controller, MessageComposerControllerError::tracker);
        invalidation = applied.invalidation;
    }
    const auto confirmed = message_send_tracker_confirm(&controller->sends, controller->timeline,
        nonce, message.channel_id, message.author_id, message.id, MessageSendConfirmationSource::response);
    if (!confirmed.ok()) return controller_error(controller, tracker_error(confirmed.error));
    request->phase = MessageComposerRequestPhase::confirmed;
    request->retry_at_ms = 0;
    // The local removal and server-confirmed insertion can change different
    // indices; invalidate the resulting loaded list as one commit.
    if (confirmed.changed) invalidation = {0, controller->timeline->store->count, true, true};
    return MessageComposerControllerResult{invalidation, nonce, MessageComposerControllerError::none, true};
}

bool message_composer_controller_observe_history(MessageComposerController* controller,
    ConstSlice<GatewayMessageEvent> messages, bool* changed) noexcept {
    if (changed) *changed = false;
    if (!changed || !controller_valid(controller) || (!messages.data && messages.count)) return false;
    constexpr u64 required = gateway_message_field_id | gateway_message_field_channel_id |
        gateway_message_field_author_id | gateway_message_field_nonce;
    for (u32 i = 0; i < messages.count; ++i) {
        const auto& message = messages[i];
        if ((message.fields & required) != required || !message.id || !message.nonce) continue;
        auto* request = find_request(controller, message.nonce);
        if (!request || request->phase == MessageComposerRequestPhase::confirmed ||
            request->channel_id != message.channel_id || request->author_id != message.author_id) continue;
        const auto observed = message_send_tracker_confirm(&controller->sends, controller->timeline,
            message.nonce, message.channel_id, message.author_id, message.id, MessageSendConfirmationSource::history);
        if (!observed.ok()) return false;
        if (observed.completed) {
            request->phase = MessageComposerRequestPhase::confirmed;
            request->retry_at_ms = 0;
            *changed = true;
        }
    }
    return true;
}

bool message_composer_controller_restore_local(MessageComposerController* controller, u64 channel_id, bool* changed) noexcept {
    if (changed) *changed = false;
    if (!changed || !controller_valid(controller) || !controller->timeline->list) return false;
    if (!channel_id || !controller->request_count || !controller->timeline->follow_bottom) return true;
    const MessageComposerRequest* selected[kMessageComposerRequestCapacity]{};
    bool failed[kMessageComposerRequestCapacity]{};
    u32 count = 0;
    for (const auto& request : controller->requests) {
        if (!request.occupied || request.channel_id != channel_id) continue;
        const MessageSendAttempt* attempt = nullptr;
        for (const auto& candidate : controller->sends.attempts)
            if (candidate.active && candidate.nonce == request.nonce) { attempt = &candidate; break; }
        if (!attempt || attempt->gateway_observed) continue;
        u32 at = count;
        while (at && selected[at - 1]->sequence > request.sequence) {
            selected[at] = selected[at - 1]; failed[at] = failed[at - 1]; --at;
        }
        selected[at] = &request; failed[at] = attempt->failed; ++count;
    }
    const auto& store = *controller->timeline->store;
    const u32 capacity = minimum(store.capacity, controller->timeline->list->capacity);
    u32 first = count > capacity ? count - capacity : 0;
    u64 text_bytes = 0;
    for (u32 i = first; i < count; ++i) text_bytes += selected[i]->text_bytes;
    while (first < count && text_bytes > store.text_capacity) text_bytes -= selected[first++]->text_bytes;
    for (u32 i = first; i < count; ++i) {
        const auto& request = *selected[i];
        const auto found = message_store_find(store, request.nonce);
        if (found.ok()) {
            const auto* record = message_store_resolve(store, found.handle);
            if (!record || record->channel_id != channel_id || record->delivery == MessageDeliveryState::authoritative) return false;
            MessageTimelineInvalidation invalidation{};
            if (!message_timeline_set_delivery(controller->timeline, request.nonce,
                failed[i] ? MessageDeliveryState::failed : MessageDeliveryState::pending, &invalidation)) return false;
            *changed = *changed || invalidation.count != 0;
        } else if (found.error != MessageStoreError::not_found || !restore_request(controller, request, failed[i])) return false;
        else *changed = true;
    }
    return true;
}
bool message_composer_controller_tick(MessageComposerController* controller, u64 now_monotonic_ms,
                                      bool* changed) noexcept {
    return controller_valid(controller) &&
           message_send_tracker_tick(&controller->sends, controller->timeline, now_monotonic_ms, changed);
}

u64 message_composer_controller_next_wake_at(const MessageComposerController& controller) noexcept {
    if (!controller_valid(&controller)) {
        return 0;
    }
    u64 wake_at = 0;
    for (u32 index = 0; index < kMessageComposerRequestCapacity; ++index) {
        const MessageComposerRequest& request = controller.requests[index];
        if (request.occupied && request.phase == MessageComposerRequestPhase::retry_wait &&
            request.retry_at_ms != 0 && (wake_at == 0 || request.retry_at_ms < wake_at)) {
            wake_at = request.retry_at_ms;
        }
    }
    for (u32 index = 0; index < kMessageSendTrackerCapacity; ++index) {
        const MessageSendAttempt& attempt = controller.sends.attempts[index];
        if (attempt.active && attempt.deadline_ms != 0 && (wake_at == 0 || attempt.deadline_ms < wake_at)) {
            wake_at = attempt.deadline_ms;
        }
    }
    return wake_at;
}

bool message_composer_controller_take_completion(MessageComposerController* controller,
                                                 MessageSendCompletion* completion) noexcept {
    if (!controller_valid(controller) || !completion ||
        !message_send_tracker_take_completion(&controller->sends, completion)) {
        return false;
    }
    MessageComposerRequest* const request = find_request(controller, completion->nonce);
    if (request) {
        if (completion->kind == MessageSendCompletionKind::accepted) {
            erase_request(controller, request);
            ++controller->completed;
        } else if (request->phase != MessageComposerRequestPhase::confirmed) {
            request->phase = MessageComposerRequestPhase::failed;
            request->retry_at_ms = 0;
            ++controller->failed;
        }
    }
    return true;
}

MessageSendTracker* message_composer_controller_send_tracker(
    MessageComposerController* controller) noexcept {
    return controller_valid(controller) ? &controller->sends : nullptr;
}

} // namespace maicord
