#include "maicord/app/pinned_message_controller.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/message_card_excerpt.hpp"

namespace maicord {
namespace {
bool scope_valid(PinnedMessageScope value) noexcept {
    return value.account_scope && value.profile_id && value.self_user_id && value.channel_id &&
        value.channel_generation;
}
bool cursor_equal(const MessagePinsCursor& a, const MessagePinsCursor& b) noexcept {
    return a.bytes == b.bytes && a.bytes <= kMessagePinsCursorMaximumBytes &&
        bytes_equal(a.timestamp, b.timestamp, a.bytes);
}
bool advance(PinnedMessageController& value) noexcept {
    if (value.revision != ~u64{0}) { ++value.revision; return true; }
    value.active = value.queued = value.authoritative = false;
    value.retry_at_ms = 0;
    value.phase = PinnedMessagePhase::retired;
    value.failure = PinnedMessageFailure::exhausted;
    return false;
}
bool usable(const PinnedMessageController& value) noexcept {
    return value.active && value.available && value.revision != ~u64{0};
}
void fetch(PinnedMessageController& value, MessagePinsCursor before = {}, u32 page_index = 0) noexcept {
    value.pending = {{value.scope.channel_id, before, kMessagePinsMaximumItems}, 0,
        MessagePinsOperation::fetch};
    value.queued = true;
    value.requested_page_index = page_index;
    value.retry_at_ms = 0;
    value.rate_attempts = 0;
    value.phase = PinnedMessagePhase::loading;
}
bool request_equal(const PinnedMessageController& value, const PinnedMessageRequest& request) noexcept {
    return request.ready && usable(value) && !value.request_id && value.queued &&
        request.revision == value.revision && pinned_message_scope_equal(value.scope, request.scope) &&
        request.payload.operation == value.pending.operation &&
        request.payload.message_id == value.pending.message_id &&
        request.payload.query.channel_id == value.pending.query.channel_id &&
        request.payload.query.limit == value.pending.query.limit &&
        cursor_equal(request.payload.query.before, value.pending.query.before);
}
u16 excerpt(char* destination, u32 capacity, Utf8View source) noexcept {
    return static_cast<u16>(utf8_copy_truncated(destination, capacity, source));
}
bool page_valid(const PinnedMessageController& value, const MessagePinsPage& page,
                ConstSlice<MessagePin> items) noexcept {
    if (page.channel_id != value.scope.channel_id || page.count != items.count ||
        page.count > value.pending.query.limit || page.count > kMessagePinsMaximumItems ||
        (!items.data && items.count) || !message_pins_cursor_valid(page.next_before) ||
        (page.has_more && (!page.count || cursor_equal(page.next_before, value.pending.query.before))))
        return false;
    if (!page.count) return !page.next_before.bytes;
    if (!cursor_equal(page.next_before, items[items.count - 1].pinned_at)) return false;
    for (u32 at = 0; at < items.count; ++at) {
        const auto& item = items[at];
        const auto& message = item.message;
        if (!message.id || message.channel_id != value.scope.channel_id ||
            !message_pins_cursor_valid(item.pinned_at, false) ||
            (value.pending.query.before.bytes && cursor_equal(item.pinned_at, value.pending.query.before)) ||
            !utf8_validate(message.content).valid || !utf8_validate(message.author_name).valid ||
            (message.snapshot.present && !utf8_validate(message.snapshot.content).valid) ||
            !utf8_validate(message.author_avatar_hash).valid ||
            (at && item.pinned_at_ms > items[at - 1].pinned_at_ms)) return false;
        for (u32 previous = 0; previous < at; ++previous)
            if (message.id == items[previous].message.id) return false;
    }
    return true;
}
void install(PinnedMessageController& value, const MessagePinsPage& page,
             ConstSlice<MessagePin> items) noexcept {
    for (u32 at = 0; at < items.count; ++at) {
        const auto& item = items[at];
        const auto& message = item.message;
        auto& row = value.rows[at];
        reset_value(&row);
        row.channel_id = message.channel_id;
        row.message_id = message.id;
        row.author_id = message.author_id;
        row.timestamp_ms = message.timestamp_ms;
        row.type = message.type;
        row.pinned_at_ms = item.pinned_at_ms;
        row.author_bytes = excerpt(row.author, sizeof(row.author), message.author_name);
        row.avatar_bytes = excerpt(row.avatar_hash, sizeof(row.avatar_hash), message.author_avatar_hash);
        row.forwarded = message.reference.type == 1 || message.snapshot.present;
        const auto card = message_card_excerpt({message.content,
            message.snapshot.present ? message.snapshot.content : Utf8View{}, row.forwarded},
            {row.content, sizeof(row.content)});
        row.content_bytes = card.bytes;
        row.attachment_count = message.attachments.count + (message.snapshot.present ? message.snapshot.attachments.count : 0);
        row.embed_count = message.embeds.count + (message.snapshot.present ? message.snapshot.embeds.count : 0);
        row.sticker_count = message.stickers.count + (message.snapshot.present ? message.snapshot.stickers.count : 0);
        row.component_count = message.components.count + (message.snapshot.present ? message.snapshot.components.count : 0);
        row.excerpt_truncated = card.truncated;
        row.rich_truncated = message.rich_truncations != 0 || message.snapshot.rich_truncations != 0;
    }
    for (u32 at = items.count; at < value.page.count; ++at) reset_value(&value.rows[at]);
    value.page = page;
    value.page_remote_revision = value.remote_revision;
    value.has_page = value.authoritative = true;
    value.page_index = value.requested_page_index;
    value.has_previous = value.page_index != 0;
    value.cursors[value.page_index] = value.pending.query.before;
    value.cursor_count = value.page_index + 1;
    bool selection_present = false;
    for (u32 at = 0; at < items.count; ++at)
        selection_present |= value.rows[at].message_id == value.selected_message_id;
    if (!selection_present) value.selected_message_id = 0;
    value.phase = PinnedMessagePhase::ready;
    value.failure = PinnedMessageFailure::none;
    value.rate_attempts = 0;
}
} // namespace
Utf8View pinned_message_author(const PinnedMessageRow& row) noexcept { return {row.author, row.author_bytes}; }
Utf8View pinned_message_avatar(const PinnedMessageRow& row) noexcept { return {row.avatar_hash, row.avatar_bytes}; }
Utf8View pinned_message_content(const PinnedMessageRow& row) noexcept { return {row.content, row.content_bytes}; }
bool pinned_message_scope_equal(PinnedMessageScope a, PinnedMessageScope b) noexcept {
    return a.account_scope == b.account_scope && a.profile_id == b.profile_id &&
        a.self_user_id == b.self_user_id && a.channel_id == b.channel_id &&
        a.channel_generation == b.channel_generation;
}
bool pinned_message_controller_open(PinnedMessageController* value, PinnedMessageScope scope,
                                    bool available, bool can_pin) noexcept {
    if (!value || value->request_id || !scope_valid(scope) || value->revision == ~u64{0}) return false;
    const u64 revision = value->revision;
    clear_bytes(value, sizeof(*value));
    value->revision = revision;
    value->scope = scope;
    value->active = true;
    value->available = available;
    value->can_pin = can_pin;
    if (available) fetch(*value);
    else { value->phase = PinnedMessagePhase::failed; value->failure = PinnedMessageFailure::permissions; }
    return advance(*value);
}
void pinned_message_controller_cancel(PinnedMessageController* value) noexcept {
    if (!value) return;
    value->active = value->queued = value->authoritative = false;
    value->retry_at_ms = value->selected_message_id = 0;
    value->phase = PinnedMessagePhase::retired;
    value->failure = PinnedMessageFailure::cancelled;
    advance(*value);
}
void pinned_message_controller_authority(PinnedMessageController* value, PinnedMessageScope scope,
                                         bool available, bool can_pin) noexcept {
    if (!value || !value->active) return;
    if (!scope_valid(scope) || !pinned_message_scope_equal(value->scope, scope)) {
        pinned_message_controller_cancel(value); return;
    }
    available = available && !value->read_denied;
    const bool was_available = value->available;
    const bool allowed = can_pin && !value->permission_denied;
    if (available == value->available && allowed == value->can_pin) return;
    value->available = available;
    value->can_pin = allowed;
    if (!available) {
        if (value->remote_revision == ~u64{0}) { pinned_message_controller_cancel(value); return; }
        ++value->remote_revision;
        value->queued = value->authoritative = false;
        value->phase = PinnedMessagePhase::failed;
        value->failure = PinnedMessageFailure::permissions;
    } else if (!was_available && !value->request_id) fetch(*value);
    else if (!allowed && value->queued && value->pending.operation != MessagePinsOperation::fetch) {
        value->queued = false;
        value->phase = PinnedMessagePhase::failed;
        value->failure = PinnedMessageFailure::permissions;
        value->authoritative = value->has_page && value->page_remote_revision == value->remote_revision;
    }
    advance(*value);
}
void pinned_message_controller_reauthorize(PinnedMessageController* value, PinnedMessageScope scope,
                                           bool available, bool can_pin) noexcept {
    if (!value || !value->active || !pinned_message_scope_equal(value->scope, scope)) return;
    value->permission_denied = !can_pin;
    value->read_denied = !available;
    pinned_message_controller_authority(value, scope, available, can_pin);
}
bool pinned_message_controller_refresh(PinnedMessageController* value) noexcept {
    if (!value || !usable(*value) || value->request_id ||
        (value->queued && value->pending.operation != MessagePinsOperation::fetch)) return false;
    fetch(*value);
    value->failure = PinnedMessageFailure::none;
    return advance(*value);
}
bool pinned_message_controller_next(PinnedMessageController* value) noexcept {
    if (!value || !usable(*value) || value->request_id || value->queued || !value->authoritative ||
        !value->has_page || !value->page.has_more) return false;
    if (value->page_index + 1 >= kPinnedMessageCursorHistory) {
        value->phase = PinnedMessagePhase::failed;
        value->failure = PinnedMessageFailure::capacity;
        advance(*value);
        return false;
    }
    fetch(*value, value->page.next_before, value->page_index + 1);
    value->failure = PinnedMessageFailure::none;
    return advance(*value);
}
bool pinned_message_controller_previous(PinnedMessageController* value) noexcept {
    if (!value || !usable(*value) || value->request_id || value->queued || !value->authoritative ||
        !value->has_page || !value->has_previous || value->page_index >= value->cursor_count) return false;
    const u32 index = value->page_index - 1;
    fetch(*value, value->cursors[index], index);
    value->failure = PinnedMessageFailure::none;
    return advance(*value);
}
bool pinned_message_controller_retry(PinnedMessageController* value) noexcept {
    if (!value || !usable(*value) || value->request_id || value->queued ||
        value->phase != PinnedMessagePhase::failed ||
        (value->pending.operation != MessagePinsOperation::fetch && !value->can_pin)) return false;
    value->queued = true;
    value->retry_at_ms = 0;
    value->rate_attempts = 0;
    value->failure = PinnedMessageFailure::none;
    value->phase = value->pending.operation == MessagePinsOperation::fetch ?
        PinnedMessagePhase::loading : PinnedMessagePhase::mutating;
    return advance(*value);
}
bool pinned_message_controller_mutate(PinnedMessageController* value, u64 message_id, bool pin) noexcept {
    if (!value || !usable(*value) || !message_id || !value->can_pin || value->request_id ||
        (value->queued && value->pending.operation != MessagePinsOperation::fetch)) return false;
    // A pin-notification system message cannot itself be pinned. Timeline
    // callers still authorize their resident message type before this entry.
    if (value->has_page) for (u32 at = 0; at < value->page.count; ++at)
        if (value->rows[at].message_id == message_id && value->rows[at].type == 6) return false;
    value->pending = {{value->scope.channel_id, {}, kMessagePinsMaximumItems}, message_id,
        pin ? MessagePinsOperation::pin : MessagePinsOperation::unpin};
    value->queued = true;
    value->authoritative = false;
    value->retry_at_ms = value->selected_message_id = 0;
    value->rate_attempts = 0;
    value->phase = PinnedMessagePhase::mutating;
    value->failure = PinnedMessageFailure::none;
    return advance(*value);
}
PinnedMessageRequest pinned_message_controller_request(const PinnedMessageController& value, u64 now_ms) noexcept {
    if (!usable(value) || !value.queued || value.request_id ||
        (value.retry_at_ms && now_ms < value.retry_at_ms) ||
        (value.pending.operation != MessagePinsOperation::fetch && !value.can_pin)) return {};
    return {value.scope, value.pending, value.revision, true};
}
bool pinned_message_controller_submitted(PinnedMessageController* value, const PinnedMessageRequest& request,
                                         u64 request_id) noexcept {
    if (!value || !request_id || !request_equal(*value, request) ||
        (value->pending.operation != MessagePinsOperation::fetch && !value->can_pin)) return false;
    value->request_id = request_id;
    value->submitted_remote_revision = value->remote_revision;
    value->queued = false;
    value->retry_at_ms = 0;
    return true;
}
bool pinned_message_controller_admission_failed(PinnedMessageController* value, const PinnedMessageRequest& request,
                                                bool busy) noexcept {
    if (!value || !request_equal(*value, request)) return false;
    if (busy) return true;
    value->queued = false;
    value->phase = PinnedMessagePhase::failed;
    value->failure = PinnedMessageFailure::admission;
    return advance(*value);
}
bool pinned_message_controller_invalidate(PinnedMessageController* value, PinnedMessageScope scope) noexcept {
    if (!value || !usable(*value) || !pinned_message_scope_equal(value->scope, scope)) return false;
    if (value->remote_revision == ~u64{0}) { pinned_message_controller_cancel(value); return false; }
    ++value->remote_revision;
    value->authoritative = false;
    value->selected_message_id = 0;
    if (!value->request_id && (!value->queued || value->pending.operation == MessagePinsOperation::fetch))
        fetch(*value);
    return advance(*value);
}
bool pinned_message_controller_complete(PinnedMessageController* value, PinnedMessageScope scope, u64 request_id,
                                        PinnedMessageCompletion completion, const MessagePinsPage* page,
                                        ConstSlice<MessagePin> items, u64 now_ms) noexcept {
    if (!value || !request_id || request_id != value->request_id ||
        !pinned_message_scope_equal(value->scope, scope)) return false;
    value->request_id = 0;
    if (!value->active) return true;
    const bool mutation = value->pending.operation != MessagePinsOperation::fetch;
    value->queued = false;
    value->retry_at_ms = 0;
    if (!value->available) {
        value->authoritative = false;
        value->phase = PinnedMessagePhase::failed;
        value->failure = PinnedMessageFailure::permissions;
    } else if (completion.kind == PinnedMessageCompletionKind::permission_lost) {
        if (mutation) { value->can_pin = false; value->permission_denied = true; }
        else { value->available = false; value->read_denied = true; value->authoritative = false; }
        value->phase = PinnedMessagePhase::failed;
        value->failure = PinnedMessageFailure::permissions;
        if (mutation) {
            value->authoritative = value->has_page && value->page_remote_revision == value->remote_revision;
            if (!value->authoritative) fetch(*value);
        }
    } else if (value->remote_revision != value->submitted_remote_revision) {
        value->failure = PinnedMessageFailure::none;
        fetch(*value);
    } else if (completion.kind == PinnedMessageCompletionKind::rate_limited) {
        value->failure = PinnedMessageFailure::rate_limited;
        if ((!mutation || value->can_pin) && value->rate_attempts < 3 &&
            completion.retry_at_ms > now_ms && completion.retry_at_ms - now_ms <= 86'400'000) {
            ++value->rate_attempts;
            value->retry_at_ms = completion.retry_at_ms;
            value->queued = true;
            value->phase = mutation ? PinnedMessagePhase::mutating : PinnedMessagePhase::loading;
        } else value->phase = PinnedMessagePhase::failed;
    } else if (completion.kind == PinnedMessageCompletionKind::success) {
        if (mutation) { value->failure = PinnedMessageFailure::none; fetch(*value); }
        else if (page && page_valid(*value, *page, items)) install(*value, *page, items);
        else { value->phase = PinnedMessagePhase::failed; value->failure = PinnedMessageFailure::response; }
    } else {
        value->failure = completion.kind == PinnedMessageCompletionKind::rejected ? PinnedMessageFailure::rejected :
            completion.kind == PinnedMessageCompletionKind::cancelled ? PinnedMessageFailure::cancelled :
            PinnedMessageFailure::ambiguous;
        if (mutation && completion.kind != PinnedMessageCompletionKind::rejected) fetch(*value);
        else {
            value->phase = PinnedMessagePhase::failed;
            if (mutation) {
                value->authoritative = value->has_page && value->page_remote_revision == value->remote_revision;
                if (!value->authoritative) fetch(*value);
            }
        }
    }
    advance(*value);
    return true;
}
const PinnedMessageRow* pinned_message_controller_row(const PinnedMessageController& value, u32 index) noexcept {
    return value.active && value.available && value.has_page && index < value.page.count ? &value.rows[index] : nullptr;
}
bool pinned_message_controller_select(PinnedMessageController* value, u64 message_id) noexcept {
    if (!value || !usable(*value) || !value->authoritative || !message_id) return false;
    for (u32 at = 0; at < value->page.count; ++at) if (value->rows[at].message_id == message_id) {
        value->selected_message_id = message_id; return advance(*value);
    }
    return false;
}
bool pinned_message_controller_mutation_pending(const PinnedMessageController& value) noexcept {
    return value.active && value.pending.operation != MessagePinsOperation::fetch &&
        (value.queued || value.request_id);
}
u64 pinned_message_controller_next_wake(const PinnedMessageController& value) noexcept {
    if (!usable(value) || !value.queued || value.request_id ||
        (value.pending.operation != MessagePinsOperation::fetch && !value.can_pin)) return 0;
    return value.retry_at_ms ? value.retry_at_ms : 1;
}
} // namespace maicord
