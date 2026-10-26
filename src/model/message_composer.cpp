#include "maicord/model/message_composer.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
u32 atomic_boundary(const MessageComposer& composer, u32 position, bool forward) noexcept {
    if (composer.atomic_revision != composer.text_revision) return position;
    for (u32 i = 0; i < composer.atomic_spans.count; ++i) {
        const auto& span = composer.atomic_spans[i];
        if (position > span.first && position < span.last) return forward ? span.last : span.first;
    }
    return position;
}
void next_document(MessageComposer* composer) noexcept {
    if (++composer->document_identity == 0) ++composer->document_identity;
}
inline TextEditHistoryState history_state(const MessageComposer& composer) noexcept {
    return {{reinterpret_cast<u8*>(composer.text), composer.capacity, composer.bytes},
        composer.anchor, composer.caret, composer.revision, composer.history};
}
void reset_history(MessageComposer* composer) noexcept {
    auto state = history_state(*composer); text_edit_reset_history(&state);
}
MessageComposerResult replay_history(MessageComposer* composer, bool forward) noexcept {
    if (!composer || !composer->text) return MessageComposerResult::invalid_input;
    auto state = history_state(*composer);
    const auto result = forward ? text_edit_redo(&state) : text_edit_undo(&state);
    composer->pending_high_surrogate = 0;
    if (result != TextEditHistoryResult::changed) return result == TextEditHistoryResult::unchanged ?
        MessageComposerResult::unchanged : MessageComposerResult::invalid_input;
    composer->bytes = state.storage.bytes; composer->anchor = state.anchor; composer->caret = state.caret; composer->revision = state.revision;
    ++composer->text_revision;
    composer->phase = MessageComposerPhase::idle; composer->http_status = 0; composer->retry_at_ms = 0;
    return MessageComposerResult::changed;
}

u32 selection_begin(const MessageComposer& composer) noexcept {
    return minimum(composer.anchor, composer.caret);
}

u32 selection_end(const MessageComposer& composer) noexcept {
    return maximum(composer.anchor, composer.caret);
}

u32 previous_boundary(const MessageComposer& composer, u32 position) noexcept {
    if (position == 0) {
        return 0;
    }
    --position;
    while (position != 0 && (static_cast<u8>(composer.text[position]) & 0xC0u) == 0x80u) {
        --position;
    }
    return atomic_boundary(composer, position, false);
}

u32 next_boundary(const MessageComposer& composer, u32 position) noexcept {
    if (position >= composer.bytes) {
        return composer.bytes;
    }
    const Utf8DecodeResult decoded = utf8_decode_one(composer.text + position, composer.bytes - position);
    return atomic_boundary(composer, decoded.valid ? position + decoded.width : composer.bytes, true);
}

bool text_boundary(const MessageComposer& composer, u32 position) noexcept {
    return position <= composer.bytes && (position == 0 || position == composer.bytes ||
                                          (static_cast<u8>(composer.text[position]) & 0xC0u) != 0x80u);
}

MessageComposerResult replace_selection(MessageComposer* composer, Utf8View inserted,
    TextEditHistoryInput input = TextEditHistoryInput::isolated, u64 time_ms = 0,
    u32 original_anchor = ~0u, u32 original_caret = ~0u) noexcept {
    if (!composer || !composer->text || composer->capacity == 0 || composer->bytes > composer->capacity ||
        composer->anchor > composer->bytes || composer->caret > composer->bytes ||
        (!inserted.data && inserted.bytes != 0)) {
        return MessageComposerResult::invalid_input;
    }
    if (!text_boundary(*composer, composer->anchor) || !text_boundary(*composer, composer->caret))
        return MessageComposerResult::invalid_input;
    // A borrowed source may be overwritten by the suffix move. Reject it
    // before recording history or changing the document.
    if (inserted.bytes != 0) {
        const usize source = reinterpret_cast<usize>(inserted.data);
        const usize destination = reinterpret_cast<usize>(composer->text);
        if (source >= destination ? source - destination < composer->capacity
                                  : destination - source < inserted.bytes)
            return MessageComposerResult::invalid_input;
    }
    const u32 begin = atomic_boundary(*composer, selection_begin(*composer), false);
    const u32 end = atomic_boundary(*composer, selection_end(*composer), true);
    const u32 retained = composer->bytes - (end - begin);
    if (inserted.bytes > composer->capacity - retained) {
        return MessageComposerResult::capacity;
    }
    if (begin == end && inserted.bytes == 0) {
        return MessageComposerResult::unchanged;
    }
    if (composer->owns_storage && !composer->history) message_composer_enable_history(composer);
    auto history = history_state(*composer);
    text_edit_history_begin(&history, begin, end - begin, inserted.bytes,
        original_anchor == ~0u ? composer->anchor : original_anchor,
        original_caret == ~0u ? composer->caret : original_caret, input, time_ms);
    copy_bytes(composer->text + begin + inserted.bytes, composer->text + end, composer->bytes - end);
    copy_bytes(composer->text + begin, inserted.data, inserted.bytes);
    const u32 old_bytes = composer->bytes;
    composer->bytes = retained + inserted.bytes;
    for (u32 index = composer->bytes; index < old_bytes; ++index) {
        composer->text[index] = 0;
    }
    composer->anchor = begin + inserted.bytes;
    composer->caret = composer->anchor;
    composer->phase = MessageComposerPhase::idle;
    composer->http_status = 0;
    composer->pending_high_surrogate = 0;
    composer->retry_at_ms = 0;
    ++composer->revision;
    text_edit_history_finish(composer->history, reinterpret_cast<const u8*>(composer->text), composer->anchor, composer->caret);
    ++composer->text_revision;
    return MessageComposerResult::changed;
}

// A typed character: keyboard control characters (Escape, Ctrl+letter,
// Ctrl+Backspace) are commands, never text.
bool scalar_allowed(u32 codepoint) noexcept {
    return codepoint >= 0x20u && codepoint != 0x7Fu && codepoint <= 0x10FFFFu &&
           !(codepoint >= 0xD800u && codepoint <= 0xDFFFu);
}

// Inserted text (paste, IME, edits, commands) keeps control characters:
// clipboard text is inserted verbatim, so ANSI escapes (U+001B), tabs and other
// C0/C1 controls reach the message. NUL cannot travel through the Windows
// clipboard, and a bare CR must already have been normalized to LF by the caller.
bool inserted_scalar_allowed(u32 codepoint) noexcept {
    return codepoint != 0 && codepoint != '\r' && codepoint <= 0x10FFFFu &&
           !(codepoint >= 0xD800u && codepoint <= 0xDFFFu);
}

u32 line_begin(const MessageComposer& composer, u32 position) noexcept {
    while (position != 0 && composer.text[position - 1] != '\n') {
        --position;
    }
    return position;
}

u32 line_end(const MessageComposer& composer, u32 position) noexcept {
    while (position < composer.bytes && composer.text[position] != '\n') {
        ++position;
    }
    return position;
}

} // namespace

bool message_composer_initialize(MessageComposer* composer, Slice<char> storage) noexcept {
    if (!composer || composer->owned_storage.base || !storage.data || storage.count == 0 ||
        storage.count > kMessageComposerMaximumBytes) {
        return false;
    }
    reset_value<MessageComposer>(composer);
    composer->text = storage.data;
    composer->document_identity = 1;
    composer->capacity = storage.count;
    for (u32 index = 0; index < storage.count; ++index) {
        storage[index] = 0;
    }
    return true;
}

bool message_composer_initialize_owned(MessageComposer* composer, u32 capacity) noexcept {
    if (!composer || composer->owned_storage.base || capacity == 0 ||
        capacity > kMessageComposerMaximumBytes) {
        return false;
    }
    reset_value<MessageComposer>(composer);
    if (!arena_initialize(&composer->owned_storage, capacity, 0, 4 * 1024)) {
        return false;
    }
    // This is the first allocation from a fresh, zero-filled reservation.
    composer->text = arena_allocate<char>(&composer->owned_storage, capacity);
    if (!composer->text) {
        arena_release(&composer->owned_storage);
        reset_value<MessageComposer>(composer);
        return false;
    }
    composer->capacity = capacity;
    composer->owns_storage = true;
    composer->document_identity = 1;
    return true;
}

void message_composer_release(MessageComposer* composer) noexcept {
    if (!composer) {
        return;
    }
    auto history = history_state(*composer); text_edit_history_release(&history);
    if (composer->owns_storage) {
        arena_reset(&composer->owned_storage, ArenaRewindMode::erase);
        arena_release(&composer->owned_storage);
    }
    reset_value<MessageComposer>(composer);
}
bool message_composer_enable_history(MessageComposer* composer, u32 budget) noexcept {
    if (!composer || !composer->text) return false;
    auto state = history_state(*composer);
    const bool result = text_edit_enable_history(&state, budget); composer->history = state.history; return result;
}
void message_composer_break_history_run(MessageComposer* composer) noexcept {
    if (!composer || !composer->history) return;
    auto state = history_state(*composer); text_edit_break_history_run(&state);
}
MessageComposerResult message_composer_undo(MessageComposer* composer) noexcept { return replay_history(composer, false); }
MessageComposerResult message_composer_redo(MessageComposer* composer) noexcept { return replay_history(composer, true); }
TextEditHistoryCosts message_composer_history_costs(const MessageComposer& composer) noexcept { return text_edit_history_costs(history_state(composer)); }

void message_composer_reset(MessageComposer* composer, u64 channel_id, bool retain_draft) noexcept {
    if (!composer || !composer->text || composer->bytes > composer->capacity) {
        return;
    }
    if (!retain_draft || channel_id != composer->channel_id) {
        reset_history(composer);
        next_document(composer);
    }
    else message_composer_break_history_run(composer);
    if (!retain_draft) {
        for (u32 index = 0; index < composer->bytes; ++index) {
            composer->text[index] = 0;
        }
        composer->bytes = 0;
        composer->anchor = 0;
        composer->caret = 0;
        ++composer->revision;
        ++composer->text_revision;
    }
    composer->pending_high_surrogate = 0;
    composer->channel_id = channel_id;
    composer->nonce = 0;
    composer->retry_at_ms = 0;
    composer->http_status = 0;
    composer->phase = MessageComposerPhase::idle;
}

bool message_composer_restore(MessageComposer* composer, u64 channel_id, const Utf8View& text_view, u32 anchor,
                              u32 caret) noexcept {
    const Utf8View text = text_view; // the composer is written below
    if (!composer || !composer->text || (!text.data && text.bytes != 0) ||
        text.bytes > composer->capacity || anchor > text.bytes || caret > text.bytes ||
        !utf8_validate(text).valid ||
        (anchor != 0 && anchor != text.bytes && (static_cast<u8>(text.data[anchor]) & 0xC0u) == 0x80u) ||
        (caret != 0 && caret != text.bytes && (static_cast<u8>(text.data[caret]) & 0xC0u) == 0x80u)) {
        return false;
    }
    const u64 source_begin = reinterpret_cast<u64>(text.data);
    const u64 source_end = source_begin + text.bytes;
    const u64 destination_begin = reinterpret_cast<u64>(composer->text);
    const u64 destination_end = destination_begin + composer->capacity;
    if (text.bytes != 0 && source_begin < destination_end && source_end > destination_begin) {
        return false;
    }
    message_composer_reset(composer, channel_id, false);
    copy_bytes(composer->text, text.data, text.bytes);
    composer->bytes = text.bytes;
    composer->anchor = anchor;
    composer->caret = caret;
    ++composer->revision;
    ++composer->text_revision;
    return true;
}

bool message_composer_refresh_borrowed(MessageComposer* composer, u32 bytes) noexcept {
    if (!composer || !composer->text || composer->owns_storage || !composer->capacity ||
        composer->capacity > kMessageComposerMaximumBytes || bytes > composer->capacity ||
        !utf8_validate({composer->text, bytes}).valid)
        return false;
    reset_history(composer);
    next_document(composer);
    composer->bytes = bytes;
    composer->anchor = minimum(composer->anchor, bytes);
    composer->caret = minimum(composer->caret, bytes);
    while (composer->anchor && composer->anchor < bytes &&
           (static_cast<u8>(composer->text[composer->anchor]) & 0xC0u) == 0x80u)
        --composer->anchor;
    while (composer->caret && composer->caret < bytes &&
           (static_cast<u8>(composer->text[composer->caret]) & 0xC0u) == 0x80u)
        --composer->caret;
    reset_value(&composer->atomic_spans);
    composer->atomic_revision = 0;
    composer->pending_high_surrogate = 0;
    composer->nonce = composer->retry_at_ms = 0;
    composer->http_status = 0;
    composer->phase = MessageComposerPhase::idle;
    ++composer->revision;
    ++composer->text_revision;
    return true;
}

Utf8View message_composer_text(const MessageComposer& composer) noexcept {
    return composer.text && composer.bytes <= composer.capacity ? Utf8View{composer.text, composer.bytes}
                                                                : Utf8View{};
}

bool message_composer_can_send(const MessageComposer& composer, bool channel_allows_send,
                               u32 maximum_codepoints, bool has_attachments) noexcept {
    if (!channel_allows_send || composer.channel_id == 0 || !composer.text ||
        composer.bytes > composer.capacity || maximum_codepoints == 0 ||
        composer.phase == MessageComposerPhase::queued || composer.phase == MessageComposerPhase::sending ||
        composer.phase == MessageComposerPhase::retry_wait) return false;
    u32 count = 0, offset = 0;
    bool has_content = has_attachments;
    while (offset < composer.bytes) {
        const auto decoded = utf8_decode_one(composer.text + offset, composer.bytes - offset);
        if (!decoded.valid || count == maximum_codepoints) return false;
        const u32 c = decoded.codepoint;
        const bool whitespace = (c >= 0x09u && c <= 0x0Du) || c == 0x20u || c == 0x85u || c == 0xA0u ||
            c == 0x1680u || (c >= 0x2000u && c <= 0x200Au) || c == 0x2028u || c == 0x2029u ||
            c == 0x202Fu || c == 0x205Fu || c == 0x3000u;
        has_content = has_content || !whitespace;
        offset += decoded.width;
        ++count;
    }
    return has_content;
}

u32 message_composer_codepoints(const MessageComposer& composer) noexcept {
    if (!composer.text || composer.bytes > composer.capacity) {
        return ~0u;
    }
    u32 count = 0;
    u32 offset = 0;
    while (offset < composer.bytes) {
        const Utf8DecodeResult decoded = utf8_decode_one(composer.text + offset, composer.bytes - offset);
        if (!decoded.valid) {
            return ~0u;
        }
        offset += decoded.width;
        ++count;
    }
    return count;
}

MessageComposerResult message_composer_insert_scalar(MessageComposer* composer, u32 codepoint, u64 time_ms) noexcept {
    if (!scalar_allowed(codepoint)) {
        return MessageComposerResult::invalid_input;
    }
    char encoded[4]{};
    const u8 bytes = utf8_encode(codepoint, encoded);
    return bytes != 0 ? replace_selection(composer, Utf8View{encoded, bytes}, TextEditHistoryInput::typing, time_ms)
                      : MessageComposerResult::invalid_input;
}

MessageComposerResult message_composer_insert_utf16(MessageComposer* composer, u16 code_unit, u64 time_ms) noexcept {
    if (!composer) {
        return MessageComposerResult::invalid_input;
    }
    if (code_unit >= 0xD800u && code_unit <= 0xDBFFu) {
        composer->pending_high_surrogate = code_unit;
        return MessageComposerResult::unchanged;
    }
    if (code_unit >= 0xDC00u && code_unit <= 0xDFFFu) {
        if (composer->pending_high_surrogate == 0) {
            return MessageComposerResult::invalid_input;
        }
        const u32 high = composer->pending_high_surrogate - 0xD800u;
        const u32 low = code_unit - 0xDC00u;
        composer->pending_high_surrogate = 0;
        return message_composer_insert_scalar(composer, 0x10000u + (high << 10) + low, time_ms);
    }
    composer->pending_high_surrogate = 0;
    return message_composer_insert_scalar(composer, code_unit, time_ms);
}

MessageComposerResult message_composer_insert_utf8(MessageComposer* composer, Utf8View text) noexcept {
    if (!text.data && text.bytes != 0) {
        return MessageComposerResult::invalid_utf8;
    }
    u32 offset = 0;
    while (offset < text.bytes) {
        const Utf8DecodeResult decoded = utf8_decode_one(text.data + offset, text.bytes - offset);
        if (!decoded.valid) return MessageComposerResult::invalid_utf8;
        if (!inserted_scalar_allowed(decoded.codepoint)) {
            return MessageComposerResult::invalid_input;
        }
        offset += decoded.width;
    }
    return replace_selection(composer, text);
}

MessageComposerResult message_composer_replace_range(MessageComposer* composer, u32 begin, u32 end,
                                                     const Utf8View& text_view) noexcept {
    const Utf8View text = text_view; // the composer is written below
    if (!composer || !composer->text || composer->bytes > composer->capacity || begin > end || end > composer->bytes ||
        !text_boundary(*composer, begin) || !text_boundary(*composer, end) ||
        (!text.data && text.bytes != 0)) {
        return MessageComposerResult::invalid_input;
    }
    u32 offset = 0;
    while (offset < text.bytes) {
        const Utf8DecodeResult decoded = utf8_decode_one(text.data + offset, text.bytes - offset);
        if (!decoded.valid || !inserted_scalar_allowed(decoded.codepoint)) {
            return MessageComposerResult::invalid_input;
        }
        offset += decoded.width;
    }
    const u32 old_anchor = composer->anchor;
    const u32 old_caret = composer->caret;
    composer->anchor = begin;
    composer->caret = end;
    const MessageComposerResult result = replace_selection(composer, text, TextEditHistoryInput::isolated, 0, old_anchor, old_caret);
    if (result == MessageComposerResult::capacity || result == MessageComposerResult::invalid_input) {
        composer->anchor = old_anchor;
        composer->caret = old_caret;
    }
    return result;
}

MessageComposerResult message_composer_insert_line_break(MessageComposer* composer) noexcept {
    return replace_selection(composer, utf8_literal("\n"));
}

MessageComposerResult message_composer_backspace(MessageComposer* composer, u64 time_ms) noexcept {
    if (!composer || !composer->text || composer->bytes > composer->capacity ||
        !text_boundary(*composer, composer->caret) || !text_boundary(*composer, composer->anchor)) {
        return MessageComposerResult::invalid_input;
    }
    const u32 original_anchor = composer->anchor, original_caret = composer->caret;
    if (composer->anchor == composer->caret) {
        if (composer->caret == 0) {
            return MessageComposerResult::unchanged;
        }
        composer->anchor = previous_boundary(*composer, composer->caret);
    }
    return replace_selection(composer, Utf8View{}, TextEditHistoryInput::backspace, time_ms, original_anchor, original_caret);
}

MessageComposerResult message_composer_delete_forward(MessageComposer* composer, u64 time_ms) noexcept {
    if (!composer || !composer->text || composer->bytes > composer->capacity ||
        !text_boundary(*composer, composer->caret) || !text_boundary(*composer, composer->anchor)) {
        return MessageComposerResult::invalid_input;
    }
    const u32 original_anchor = composer->anchor, original_caret = composer->caret;
    if (composer->anchor == composer->caret) {
        if (composer->caret == composer->bytes) {
            return MessageComposerResult::unchanged;
        }
        composer->anchor = next_boundary(*composer, composer->caret);
    }
    return replace_selection(composer, Utf8View{}, TextEditHistoryInput::delete_forward, time_ms, original_anchor, original_caret);
}

MessageComposerResult message_composer_move(MessageComposer* composer, MessageComposerMove movement,
                                            bool extend_selection) noexcept {
    if (!composer || !composer->text || composer->bytes > composer->capacity ||
        !text_boundary(*composer, composer->caret) || !text_boundary(*composer, composer->anchor)) {
        return MessageComposerResult::invalid_input;
    }
    u32 position = composer->caret;
    message_composer_break_history_run(composer);
    // Plain arrows collapse selections; Home/End and word commands still move
    // to their requested boundary, preserving the caret's active end.
    if (!extend_selection && composer->anchor != composer->caret &&
        (movement == MessageComposerMove::previous || movement == MessageComposerMove::next)) {
        position = movement == MessageComposerMove::previous ? selection_begin(*composer) : selection_end(*composer);
    } else {
        switch (movement) {
        case MessageComposerMove::previous:
            position = previous_boundary(*composer, position);
            break;
        case MessageComposerMove::next:
            position = next_boundary(*composer, position);
            break;
        case MessageComposerMove::line_home:
            position = line_begin(*composer, position);
            break;
        case MessageComposerMove::line_end:
            position = line_end(*composer, position);
            break;
        case MessageComposerMove::document_home:
            position = 0;
            break;
        case MessageComposerMove::document_end:
            position = composer->bytes;
            break;
        case MessageComposerMove::previous_word:
        case MessageComposerMove::next_word:
            if (!utf8_word_boundary(message_composer_text(*composer), position, movement == MessageComposerMove::next_word, &position))
                return MessageComposerResult::invalid_input;
            break;
        default: return MessageComposerResult::invalid_input;
        }
    }
    position = atomic_boundary(*composer, position, position > composer->caret);
    const bool changed = position != composer->caret || (!extend_selection && composer->anchor != position);
    composer->caret = position;
    composer->pending_high_surrogate = 0;
    if (!extend_selection) {
        composer->anchor = position;
    }
    if (changed) {
        ++composer->revision;
        return MessageComposerResult::changed;
    }
    return MessageComposerResult::unchanged;
}

MessageComposerResult message_composer_set_selection(MessageComposer* composer, u32 anchor,
                                                     u32 caret) noexcept {
    if (!composer || !composer->text || composer->bytes > composer->capacity || anchor > composer->bytes || caret > composer->bytes ||
        !text_boundary(*composer, anchor) || !text_boundary(*composer, caret)) {
        return MessageComposerResult::invalid_input;
    }
    message_composer_break_history_run(composer);
    if (composer->anchor == anchor && composer->caret == caret) {
        return MessageComposerResult::unchanged;
    }
    composer->anchor = anchor;
    composer->caret = caret;
    composer->pending_high_surrogate = 0;
    ++composer->revision;
    return MessageComposerResult::changed;
}

MessageComposerResult message_composer_delete_word(MessageComposer* composer, bool forward, u64 time_ms) noexcept {
    if (!composer || !composer->text || composer->bytes > composer->capacity ||
        !text_boundary(*composer, composer->caret) || !text_boundary(*composer, composer->anchor))
        return MessageComposerResult::invalid_input;
    const u32 anchor = composer->anchor, caret = composer->caret;
    if (anchor == caret && !utf8_word_boundary(message_composer_text(*composer), caret, forward, &composer->anchor))
        return MessageComposerResult::invalid_input;
    message_composer_break_history_run(composer);
    const auto result = replace_selection(composer, {}, forward ? TextEditHistoryInput::delete_forward : TextEditHistoryInput::backspace,
        time_ms, anchor, caret);
    if (result != MessageComposerResult::changed) { composer->anchor = anchor; composer->caret = caret; }
    message_composer_break_history_run(composer);
    return result;
}

MessageComposerResult message_composer_select_word(MessageComposer* composer, u32 position) noexcept {
    if (!composer || !composer->text || composer->bytes > composer->capacity ||
        position > composer->bytes || !text_boundary(*composer, position)) {
        return MessageComposerResult::invalid_input;
    }
    u32 begin = 0, end = 0;
    if (!utf8_word_range_near({composer->text, composer->bytes}, position, &begin, &end))
        return MessageComposerResult::invalid_input;
    return message_composer_select_range(composer, begin, end);
}

MessageComposerResult message_composer_select_range(MessageComposer* composer, u32 anchor, u32 caret) noexcept {
    if (!composer || !composer->text || composer->bytes > composer->capacity || anchor > composer->bytes ||
        caret > composer->bytes)
        return MessageComposerResult::invalid_input;
    // Mentions, channels and custom emoji are stored as tokens but shown as
    // one unit: a range that touches one selects the whole token.
    const bool forward = anchor <= caret;
    anchor = atomic_boundary(*composer, anchor, !forward);
    caret = atomic_boundary(*composer, caret, forward);
    return message_composer_set_selection(composer, anchor, caret);
}

MessageComposerResult message_composer_select_all(MessageComposer* composer) noexcept {
    if (!composer || !composer->text || composer->bytes > composer->capacity) {
        return MessageComposerResult::invalid_input;
    }
    message_composer_break_history_run(composer);
    const bool changed = composer->anchor != 0 || composer->caret != composer->bytes;
    composer->anchor = 0;
    composer->caret = composer->bytes;
    composer->pending_high_surrogate = 0;
    if (changed) {
        ++composer->revision;
    }
    return changed ? MessageComposerResult::changed : MessageComposerResult::unchanged;
}

MessageComposerResult message_composer_clear(MessageComposer* composer) noexcept {
    if (!composer || !composer->text || composer->bytes > composer->capacity) {
        return MessageComposerResult::invalid_input;
    }
    reset_history(composer);
    next_document(composer);
    composer->pending_high_surrogate = 0;
    if (composer->bytes == 0 && composer->anchor == 0 && composer->caret == 0) {
        return MessageComposerResult::unchanged;
    }
    for (u32 index = 0; index < composer->bytes; ++index) {
        composer->text[index] = 0;
    }
    composer->bytes = 0;
    composer->anchor = 0;
    composer->caret = 0;
    composer->phase = MessageComposerPhase::idle;
    composer->nonce = 0;
    composer->retry_at_ms = 0;
    composer->http_status = 0;
    ++composer->revision;
    ++composer->text_revision;
    return MessageComposerResult::changed;
}

float message_composer_preferred_height(const MessageComposer& composer,
                                        float available_text_width) noexcept {
    const float usable_width = maximum(available_text_width, 120.0f);
    const u32 columns = maximum(1u, static_cast<u32>(usable_width / 8.0f));
    u32 lines = 1;
    u32 column = 0;
    u32 offset = 0;
    while (offset < composer.bytes) {
        const Utf8DecodeResult decoded = utf8_decode_one(composer.text + offset, composer.bytes - offset);
        if (!decoded.valid) {
            break;
        }
        if (decoded.codepoint == '\n' || ++column >= columns) {
            ++lines;
            column = 0;
        }
        offset += decoded.width;
    }
    const float height = 20.0f + static_cast<float>(minimum(lines, 8u)) * 20.0f;
    return minimum(maximum(height, kMessageComposerMinimumHeight), kMessageComposerMaximumHeight);
}

} // namespace maicord
