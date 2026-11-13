#include "maicord/ui/chat/messages/text_selection.hpp"

#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/chat/message_store.hpp"
#include "maicord/ui/chat/messages/scene.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"
#include "maicord/ui/text/pointer_selection.hpp"

namespace maicord {

struct MessageTextSelectionCommand {
    Utf8View text;
    ConstSlice<MessageMarkdownSemantic> semantics;
    u32 copy_end;
    u16 source_id;
    u16 nth;
    u8 kind;
    u8 slot;
    bool same_line; // beside the previous command of its message, on one line
};

namespace {

// Virtual reservations, committed as the selection grows: the kept list holds four selections' worth of messages; the
// text and copy reserve the capacity.
constexpr usize kMessagesReserve = 4ull * kMessageTextSelectionMaximumMessages * sizeof(MessageTextSelectionMessage);
constexpr usize kCommitStep = 64 * 1024;

bool same_key(const MessageTextCommand& command, u16 source_id, u8 kind, u8 slot) noexcept {
    return command.source_id == source_id && static_cast<u8>(command.measurement_kind) == kind &&
           command.measurement_slot == slot;
}

bool point_on(const MessageTextSelectionPoint& point, const MessageTextSelectionPoint& other) noexcept {
    return point.message_id == other.message_id && point.message_revision == other.message_revision &&
           point.source_id == other.source_id && point.kind == other.kind && point.slot == other.slot &&
           point.nth == other.nth;
}

bool points_equal(const MessageTextSelectionPoint& left, const MessageTextSelectionPoint& right) noexcept {
    return point_on(left, right) && left.byte_offset == right.byte_offset;
}

u64 point_hash(u64 hash, const MessageTextSelectionPoint& point) noexcept {
    hash = hash64_mix(hash, point.message_id);
    hash = hash64_mix(hash, (static_cast<u64>(point.message_revision) << 32) | point.byte_offset);
    return hash64_mix(hash, (static_cast<u64>(point.source_id) << 32) | (static_cast<u64>(point.nth) << 16) |
                                (static_cast<u64>(point.kind) << 8) | point.slot);
}

u64 range_signature(const MessageTextSelectionRange& range) noexcept {
    const u64 hash = point_hash(point_hash(kHash64Seed, range.anchor), range.focus);
    return hash ? hash : 1;
}

// The first command presented for messages after `id` (`inclusive`: from `id` on); the count when none is.
u32 first_command_after(const MessageTextPresentation& presentation, u64 id, bool inclusive) noexcept {
    for (u32 index = 0; index < presentation.command_count; ++index) {
        const u64 message = presentation.commands[index].message_id;
        if (message != 0 && (message > id || (inclusive && message == id)))
            return index;
    }
    return presentation.command_count;
}

// The last of a message's commands from its first, `begin`: a row keeps them together (its own, then a forwarded
// snapshot's); commands of no message (the unread divider's label) may sit among them.
u32 message_run_end(const MessageTextPresentation& presentation, u32 begin) noexcept {
    const u64 id = presentation.commands[begin].message_id;
    u32 end = begin;
    for (u32 index = begin + 1; index < presentation.command_count; ++index) {
        const u64 message = presentation.commands[index].message_id;
        if (message == id)
            end = index;
        else if (message != 0)
            break;
    }
    return end;
}

bool presentation_valid(const MessageTextPresentation& presentation) noexcept {
    return presentation.active && presentation.command_count <= presentation.commands.count &&
           (presentation.commands.data || presentation.command_count == 0);
}

MessageTextSelectionMessage* recorded_messages(const MessageTextSelectionText& text) noexcept {
    return reinterpret_cast<MessageTextSelectionMessage*>(text.messages.base);
}

// Where `id` is or would be in the recorded messages.
u32 recorded_position(const MessageTextSelectionText& text, u64 id) noexcept {
    const MessageTextSelectionMessage* messages = recorded_messages(text);
    u32 low = 0, high = text.count;
    while (low < high) {
        const u32 middle = low + (high - low) / 2;
        if (messages[middle].message_id < id)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

const MessageTextSelectionMessage* recorded_message(const MessageTextSelectionText& text, u64 id,
                                                    u32 revision) noexcept {
    if (!text.messages.base)
        return nullptr;
    const u32 at = recorded_position(text, id);
    const MessageTextSelectionMessage* message = at < text.count ? recorded_messages(text) + at : nullptr;
    return message && message->message_id == id && message->message_revision == revision ? message : nullptr;
}

// The recorded command a point names, or kMessageTextSelectionNoCommand.
u32 recorded_command(const MessageTextSelectionMessage& message, const MessageTextSelectionPoint& point) noexcept {
    for (u32 index = 0; index < message.command_count; ++index) {
        const MessageTextSelectionCommand& command = message.commands[index];
        if (command.source_id == point.source_id && command.kind == point.kind && command.slot == point.slot &&
            command.nth == point.nth)
            return point.byte_offset <= command.text.bytes ? index : kMessageTextSelectionNoCommand;
    }
    return kMessageTextSelectionNoCommand;
}

bool beside(const MessageTextCommand* previous, const MessageTextCommand& command) noexcept {
    return previous && command.rectangle.top < previous->rectangle.bottom - 0.5f &&
           command.rectangle.left >= previous->rectangle.right - 0.5f;
}

// Whether a message's recorded copy still is what the presentation shows: its commands' text, kind and line layout.
// Scrolls and rebuilds that keep them (new widgets, moved rows) record nothing again.
bool recorded_current(const MessageTextSelectionMessage& message, const MessageTextPresentation& presentation,
                      u32 begin, u32 end) noexcept {
    const u64 id = presentation.commands[begin].message_id;
    if (message.message_revision != presentation.commands[begin].message_revision)
        return false;
    u32 recorded = 0;
    const MessageTextCommand* previous = nullptr;
    for (u32 index = begin; index <= end; ++index) {
        const MessageTextCommand& command = presentation.commands[index];
        if (command.message_id != id || !message_text_command_selectable(command))
            continue;
        if (recorded >= message.command_count)
            return false;
        const MessageTextSelectionCommand& kept = message.commands[recorded++];
        if (kept.source_id != command.source_id || kept.kind != static_cast<u8>(command.measurement_kind) ||
            kept.slot != command.measurement_slot || kept.same_line != beside(previous, command) ||
            kept.text.bytes != command.text.bytes || kept.semantics.count != command.semantics.count ||
            !bytes_equal(kept.text.data, command.text.data, command.text.bytes) ||
            !bytes_equal(kept.semantics.data, command.semantics.data,
                         command.semantics.count * sizeof(MessageMarkdownSemantic)))
            return false;
        previous = &command;
    }
    return recorded == message.command_count;
}

usize capacity_of(const MessageTextSelectionText& text) noexcept {
    return text.capacity ? text.capacity : kMessageTextSelectionTextBytes;
}

// Copies a message's selectable commands, `begin..end`, into the data arena; null when they do not fit in the half
// of the capacity kept text may use (the copy's output has the other).
MessageTextSelectionCommand* record_run(MessageTextSelectionText* text, const MessageTextPresentation& presentation,
                                        u32 begin, u32 end, u32* recorded) noexcept {
    const u64 id = presentation.commands[begin].message_id;
    u32 count = 0;
    usize needed = 0;
    for (u32 index = begin; index <= end; ++index) {
        const MessageTextCommand& command = presentation.commands[index];
        if (command.message_id != id || !message_text_command_selectable(command))
            continue;
        ++count;
        needed += sizeof(MessageTextSelectionCommand) + command.text.bytes +
                  command.semantics.count * sizeof(MessageMarkdownSemantic) + 2 * alignof(MessageMarkdownSemantic);
    }
    *recorded = count;
    if (count == 0)
        return nullptr;
    if (needed > capacity_of(*text) / 2 || text->data.offset > capacity_of(*text) / 2 - needed)
        return nullptr;
    auto* commands = arena_allocate<MessageTextSelectionCommand>(&text->data, count);
    if (!commands)
        return nullptr;
    u32 written = 0;
    const MessageTextCommand* previous = nullptr;
    for (u32 index = begin; index <= end; ++index) {
        const MessageTextCommand& command = presentation.commands[index];
        if (command.message_id != id || !message_text_command_selectable(command))
            continue;
        char* bytes = static_cast<char*>(arena_allocate(&text->data, command.text.bytes, 1));
        auto* semantics = command.semantics.count
                              ? arena_allocate<MessageMarkdownSemantic>(&text->data, command.semantics.count)
                              : nullptr;
        if (!bytes || (command.semantics.count && !semantics))
            return nullptr;
        copy_bytes(bytes, command.text.data, command.text.bytes);
        if (semantics)
            copy_bytes(semantics, command.semantics.data,
                       static_cast<u32>(command.semantics.count * sizeof(MessageMarkdownSemantic)));
        MessageTextSelectionCommand& out = commands[written];
        out.text = {bytes, command.text.bytes};
        out.semantics = {semantics, command.semantics.count};
        out.copy_end = message_text_command_copy_end(command);
        out.source_id = command.source_id;
        out.kind = static_cast<u8>(command.measurement_kind);
        out.slot = command.measurement_slot;
        out.nth = 0;
        for (u32 earlier = 0; earlier < written; ++earlier)
            if (commands[earlier].source_id == out.source_id && commands[earlier].kind == out.kind &&
                commands[earlier].slot == out.slot)
                ++out.nth;
        out.same_line = beside(previous, command);
        previous = &command;
        ++written;
    }
    return commands;
}

// Drops the kept messages outside low..high, which no selection end reaches any more.
void forget_outside(MessageTextSelectionText* text, u64 low, u64 high) noexcept {
    MessageTextSelectionMessage* messages = recorded_messages(*text);
    u32 kept = 0;
    for (u32 index = 0; index < text->count; ++index)
        if (messages[index].message_id >= low && messages[index].message_id <= high)
            messages[kept++] = messages[index];
    text->count = kept;
    arena_rewind(&text->messages, ArenaMark{kept * sizeof(MessageTextSelectionMessage)});
}

bool record_message(MessageTextSelectionText* text, const MessageTextPresentation& presentation, u32 begin,
                    u32 end, u64 low, u64 high) noexcept {
    const MessageTextCommand& first = presentation.commands[begin];
    u32 at = recorded_position(*text, first.message_id);
    MessageTextSelectionMessage* existing =
        at < text->count && recorded_messages(*text)[at].message_id == first.message_id ? recorded_messages(*text) + at
                                                                                        : nullptr;
    if (existing && recorded_current(*existing, presentation, begin, end))
        return true;
    const ArenaMark mark = arena_mark(&text->data);
    u32 count = 0;
    const MessageTextSelectionCommand* commands = record_run(text, presentation, begin, end, &count);
    if (count && !commands) {
        arena_rewind(&text->data, mark);
        return false;
    }
    if (!existing) {
        // The kept list holds four selections' worth; past that, messages a drag left behind make room.
        if (!arena_allocate<MessageTextSelectionMessage>(&text->messages)) {
            forget_outside(text, low, high);
            at = recorded_position(*text, first.message_id);
            if (!arena_allocate<MessageTextSelectionMessage>(&text->messages)) {
                arena_rewind(&text->data, mark);
                return false;
            }
        }
        MessageTextSelectionMessage* messages = recorded_messages(*text);
        copy_bytes(messages + at + 1, messages + at,
                   static_cast<u32>((text->count - at) * sizeof(MessageTextSelectionMessage)));
        existing = messages + at;
        ++text->count;
    }
    *existing = MessageTextSelectionMessage{first.message_id, commands, first.message_revision, count};
    return true;
}

// Appends to the copy's output, which runs on from `start` in the data arena.
char* output_take(MessageTextSelectionText* text, u32 bytes) noexcept {
    return bytes ? static_cast<char*>(arena_allocate(&text->data, bytes, 1)) : nullptr;
}

void rewind_copy(MessageTextSelectionText* text) noexcept {
    if (text->copy_mark) {
        arena_rewind(&text->data, ArenaMark{text->copy_mark - 1});
        text->copy_mark = 0;
    }
}

} // namespace

bool message_text_command_selectable(const MessageTextCommand& command) noexcept {
    return command.message_id != 0 && command.text.bytes != 0 && command.text.data &&
           command.measurement_kind != MessageTextMeasurementKind::reaction;
}

bool message_text_selection_nonempty(const MessageTextSelectionRange& range) noexcept {
    return message_text_selection_active(range) && !points_equal(range.anchor, range.focus);
}

bool message_text_selection_point(const MessageTextPresentation& presentation, u32 command_index, u32 byte_offset,
                                  MessageTextSelectionPoint* point) noexcept {
    if (!point || !presentation_valid(presentation) || command_index >= presentation.command_count)
        return false;
    const MessageTextCommand& command = presentation.commands[command_index];
    if (!message_text_command_selectable(command) || byte_offset > command.text.bytes)
        return false;
    const u8 kind = static_cast<u8>(command.measurement_kind);
    u16 nth = 0;
    for (u32 index = command_index; index != 0; --index) {
        const MessageTextCommand& earlier = presentation.commands[index - 1];
        if (earlier.message_id == 0)
            continue;
        if (earlier.message_id != command.message_id)
            break;
        if (message_text_command_selectable(earlier) && same_key(earlier, command.source_id, kind, command.measurement_slot))
            ++nth;
    }
    *point = MessageTextSelectionPoint{command.message_id, command.message_revision, byte_offset, command.source_id,
                                       nth, kind, command.measurement_slot, 0};
    return true;
}

u32 message_text_selection_find(const MessageTextPresentation& presentation,
                                const MessageTextSelectionPoint& point) noexcept {
    if (point.message_id == 0 || !presentation_valid(presentation))
        return kMessageTextSelectionNoCommand;
    const u32 begin = first_command_after(presentation, point.message_id, true);
    if (begin >= presentation.command_count || presentation.commands[begin].message_id != point.message_id)
        return kMessageTextSelectionNoCommand;
    const u32 end = message_run_end(presentation, begin);
    u16 nth = 0;
    for (u32 index = begin; index <= end; ++index) {
        const MessageTextCommand& command = presentation.commands[index];
        if (command.message_id != point.message_id || !message_text_command_selectable(command))
            continue;
        if (command.message_revision != point.message_revision)
            return kMessageTextSelectionNoCommand;
        if (!same_key(command, point.source_id, point.kind, point.slot))
            continue;
        if (nth++ == point.nth)
            return point.byte_offset <= command.text.bytes ? index : kMessageTextSelectionNoCommand;
    }
    return kMessageTextSelectionNoCommand;
}

bool message_text_selection_begin(MessageTextSelectionRange* range, UiTextPointerSelection* unit,
                                  const MessageTextPresentation& presentation, u32 command_index, u32 byte_offset,
                                  u8 clicks) noexcept {
    MessageTextSelectionPoint point{};
    if (!range || !unit || !message_text_selection_point(presentation, command_index, byte_offset, &point))
        return false;
    u32 anchor = byte_offset, caret = byte_offset;
    *unit = {byte_offset, byte_offset, UiTextSelectionUnit::character};
    // Double click selects the word, triple click the paragraph (line).
    if (clicks > 1 && !ui_text_pointer_press(presentation.commands[command_index].text, byte_offset, clicks, false,
                                             byte_offset, false, unit, &anchor, &caret)) {
        anchor = caret = byte_offset;
        *unit = {byte_offset, byte_offset, UiTextSelectionUnit::character};
    }
    range->anchor = point;
    range->anchor.byte_offset = anchor;
    range->focus = point;
    range->focus.byte_offset = caret;
    range->signature = range_signature(*range);
    return true;
}

bool message_text_selection_extend(MessageTextSelectionRange* range, const UiTextPointerSelection& unit,
                                   const MessageTextPresentation& presentation, const MessageStore& store,
                                   u32 command_index, u32 byte_offset) noexcept {
    MessageTextSelectionPoint point{};
    if (!range || !message_text_selection_active(*range) ||
        !message_text_selection_point(presentation, command_index, byte_offset, &point))
        return false;
    // At most kMessageTextSelectionMaximumMessages messages from the anchor's (or where it was) to the focus's.
    const u32 anchor_order = message_store_locate(store, range->anchor.message_id).order_index;
    const u32 focus_order = message_store_locate(store, point.message_id).order_index;
    if ((anchor_order > focus_order ? anchor_order - focus_order : focus_order - anchor_order) >=
        kMessageTextSelectionMaximumMessages)
        return false;
    MessageTextSelectionRange next = *range;
    next.focus = point;
    if (point_on(point, range->anchor)) {
        if (unit.unit != UiTextSelectionUnit::character) {
            u32 anchor = 0, caret = 0;
            if (!ui_text_pointer_drag(presentation.commands[command_index].text, unit, byte_offset, false, &anchor,
                                      &caret))
                return false;
            next.anchor.byte_offset = anchor;
            next.focus.byte_offset = caret;
        }
    } else if (unit.unit != UiTextSelectionUnit::character) {
        // Past the anchor's command the whole first unit stays selected.
        const u32 anchor_index = message_text_selection_find(presentation, range->anchor);
        const bool forward = anchor_index != kMessageTextSelectionNoCommand
                                 ? anchor_index < command_index
                                 : range->anchor.message_id <= point.message_id;
        next.anchor.byte_offset = forward ? unit.origin_first : unit.origin_last;
    }
    if (points_equal(next.anchor, range->anchor) && points_equal(next.focus, range->focus))
        return false;
    next.signature = range_signature(next);
    *range = next;
    return true;
}

MessageTextSelectionSpan message_text_selection_span(const MessageTextSelectionRange& range,
                                                     const MessageTextPresentation& presentation) noexcept {
    if (!message_text_selection_nonempty(range) || !presentation_valid(presentation) || presentation.command_count == 0)
        return {};
    const u32 anchor = message_text_selection_find(presentation, range.anchor);
    const u32 focus = message_text_selection_find(presentation, range.focus);
    bool forward = range.anchor.message_id < range.focus.message_id;
    if (range.anchor.message_id == range.focus.message_id) {
        // Both ends in one message: nothing to show unless it is presented as they knew it.
        if (anchor == kMessageTextSelectionNoCommand || focus == kMessageTextSelectionNoCommand)
            return {};
        forward = anchor < focus || (anchor == focus && range.anchor.byte_offset <= range.focus.byte_offset);
    }
    const MessageTextSelectionPoint& start = forward ? range.anchor : range.focus;
    const MessageTextSelectionPoint& end = forward ? range.focus : range.anchor;
    const u32 start_index = forward ? anchor : focus;
    const u32 end_index = forward ? focus : anchor;
    // An end whose message is not presented, or changed, bounds the selection at that message.
    MessageTextSelectionSpan span{};
    if (start_index != kMessageTextSelectionNoCommand) {
        span.first = start_index;
        span.first_offset = start.byte_offset;
    } else {
        span.first = first_command_after(presentation, start.message_id, false);
    }
    u32 last = 0, last_offset = 0;
    if (end_index != kMessageTextSelectionNoCommand) {
        last = end_index;
        last_offset = end.byte_offset;
    } else {
        last = first_command_after(presentation, end.message_id, true);
    }
    if (last_offset == 0) {
        // Up to the start of a command: the one before it is the last, whole.
        if (last == 0)
            return {};
        --last;
        last_offset = presentation.commands[last].text.bytes;
    }
    span.last = last;
    span.last_offset = last_offset;
    span.valid = span.first < span.last || (span.first == span.last && span.first_offset < span.last_offset);
    return span.valid ? span : MessageTextSelectionSpan{};
}

float message_text_selection_autoscroll(const UiRect& viewport, float y, u64 elapsed_ms) noexcept {
    // From a crawl at the edge to a fast scroll 120 DIPs past it.
    constexpr float kSlowest = 240.0f, kFastest = 2'400.0f, kRamp = 120.0f;
    if (!ui_rect_has_area(viewport) || elapsed_ms == 0)
        return 0.0f;
    const float past = y < viewport.top ? y - viewport.top : y > viewport.bottom ? y - viewport.bottom : 0.0f;
    if (past == 0.0f)
        return 0.0f;
    const float depth = minimum((past < 0.0f ? -past : past) / kRamp, 1.0f);
    const float seconds = static_cast<float>(minimum<u64>(elapsed_ms, 100)) / 1'000.0f;
    const float distance = (kSlowest + (kFastest - kSlowest) * depth) * seconds;
    return past < 0.0f ? -distance : distance;
}

bool message_text_selection_note(MessageTextSelectionText* text, const MessageTextSelectionRange& range,
                                 const MessageTextPresentation& presentation) noexcept {
    if (!text)
        return false;
    rewind_copy(text);
    // A collapsed selection keeps its message too: a scroll away before the drag must not lose the anchor's text.
    if (!message_text_selection_active(range) || !presentation_valid(presentation))
        return true;
    if (text->messages.base && text->noted_signature == range.signature && text->noted_builds == presentation.builds)
        return !text->incomplete;
    if (!text->messages.base &&
        (!arena_initialize(&text->messages, kMessagesReserve, 0, kCommitStep, ArenaTag::ui) ||
         !arena_initialize(&text->data, capacity_of(*text), 0, kCommitStep, ArenaTag::ui))) {
        message_text_selection_text_release(text);
        return false;
    }
    const u64 low = minimum(range.anchor.message_id, range.focus.message_id);
    const u64 high = maximum(range.anchor.message_id, range.focus.message_id);
    bool complete = true;
    for (u32 index = first_command_after(presentation, low, true); index < presentation.command_count; ++index) {
        const u64 id = presentation.commands[index].message_id;
        if (id == 0)
            continue;
        if (id > high)
            break;
        const u32 end = message_run_end(presentation, index);
        complete = record_message(text, presentation, index, end, low, high) && complete;
        index = end;
    }
    text->incomplete = !complete;
    text->noted_signature = range.signature;
    text->noted_builds = presentation.builds;
    return complete;
}

void message_text_selection_text_release(MessageTextSelectionText* text) noexcept {
    if (!text)
        return;
    if (text->messages.base)
        arena_release(&text->messages);
    if (text->data.base)
        arena_release(&text->data);
    const usize capacity = text->capacity;
    reset_value(text);
    text->capacity = capacity;
}

Utf8View message_text_selection_copy(MessageTextSelectionText* text, const MessageTextSelectionRange& range,
                                     const MessageStore& store, const MessageRichStore* rich_store,
                                     const MessageScene* scene, bool* overflowed) noexcept {
    if (overflowed)
        *overflowed = false;
    if (!text)
        return {};
    rewind_copy(text);
    if (!message_text_selection_nonempty(range) || !text->messages.base || !store.records || !store.order)
        return {};
    // A selected message whose text did not fit: nothing rather than less than the selection shows.
    if (text->incomplete) {
        if (overflowed)
            *overflowed = true;
        return {};
    }
    // Order the ends: by message, within one message by its commands as presented.
    bool forward = range.anchor.message_id < range.focus.message_id;
    if (range.anchor.message_id == range.focus.message_id) {
        const auto* message = recorded_message(*text, range.anchor.message_id, range.anchor.message_revision);
        const u32 anchor = message ? recorded_command(*message, range.anchor) : kMessageTextSelectionNoCommand;
        const u32 focus = message ? recorded_command(*message, range.focus) : kMessageTextSelectionNoCommand;
        if (anchor == kMessageTextSelectionNoCommand || focus == kMessageTextSelectionNoCommand)
            return {};
        forward = anchor < focus || (anchor == focus && range.anchor.byte_offset <= range.focus.byte_offset);
    }
    const MessageTextSelectionPoint& start = forward ? range.anchor : range.focus;
    const MessageTextSelectionPoint& end = forward ? range.focus : range.anchor;
    const usize origin = text->data.offset;
    u32 written = 0;
    bool overflow = false;
    const auto append = [&](const char* bytes, u32 count) noexcept {
        char* out = output_take(text, count);
        if (count && !out) {
            overflow = true;
            return;
        }
        copy_bytes(out, bytes, count);
        written += count;
    };
    for (u32 order = message_store_locate(store, start.message_id).order_index; order < store.count && !overflow;
         ++order) {
        const MessageHandle handle = message_store_ordered_handle(store, order);
        const MessageRecord* record = message_store_resolve(store, handle);
        if (!record)
            continue;
        if (record->id > end.message_id)
            break;
        const MessageTextSelectionMessage* message = recorded_message(*text, record->id, record->revision);
        const bool at_start = record->id == start.message_id, at_end = record->id == end.message_id;
        u32 first = 0, first_offset = 0;
        u32 last = message && message->command_count ? message->command_count - 1 : 0;
        u32 last_offset = message && message->command_count ? message->commands[last].text.bytes : 0;
        if (at_start) {
            // A changed end message is a boundary: the selection keeps what lies past it.
            first = message && start.message_revision == record->revision ? recorded_command(*message, start)
                                                                            : kMessageTextSelectionNoCommand;
            if (first == kMessageTextSelectionNoCommand)
                continue;
            first_offset = start.byte_offset;
        }
        if (at_end) {
            last = message && end.message_revision == record->revision ? recorded_command(*message, end)
                                                                        : kMessageTextSelectionNoCommand;
            if (last == kMessageTextSelectionNoCommand)
                continue;
            last_offset = end.byte_offset;
        }
        if (!message) {
            // Inside the selection but never presented while selected: its stored text, unless the row hides it.
            if (at_start || at_end || !scene || scene->content_gated ||
                record->content_availability != MessageContentAvailability::available ||
                message_scene_plugin_visibility(*scene, *record) != ui_plugin_render_normal)
                continue;
            const Utf8View content = message_store_content(store, handle);
            if (content.bytes == 0)
                continue;
            if (written)
                append("\n", 1);
            append(content.data, content.bytes);
            continue;
        }
        bool message_started = false;
        for (u32 index = first; index <= last && index < message->command_count && !overflow; ++index) {
            const MessageTextSelectionCommand& recorded = message->commands[index];
            const u32 from = index == first ? first_offset : 0;
            const u32 to = minimum(index == last ? last_offset : recorded.text.bytes, recorded.copy_end);
            if (from >= to || to > recorded.text.bytes)
                continue;
            MessageTextCommand command{};
            command.text = recorded.text;
            command.semantics = recorded.semantics;
            command.message_id = record->id;
            command.message_revision = record->revision;
            command.source_id = recorded.source_id;
            const Utf8View source = message_text_presentation_command_source(store, rich_store, command);
            // As a single message's copy: text whose source is gone is not copied.
            if (command.source_id != 0 && source.bytes == 0)
                continue;
            if (written)
                append(message_started && recorded.same_line ? " " : "\n", 1);
            // Chips copy their source: room for every source in the range besides the shown text.
            u32 bound = to - from;
            for (u32 s = 0; s < recorded.semantics.count; ++s)
                bound += recorded.semantics[s].source_bytes;
            char* out = output_take(text, bound);
            if (!out) {
                overflow = true;
                break;
            }
            const MessageTextCopyProjectionResult projection =
                source.bytes ? message_text_project_selection_for_copy(command, source, from, to, Slice<char>{out, bound})
                             : MessageTextCopyProjectionResult{};
            u32 bytes = projection.ok ? projection.bytes : to - from;
            if (!projection.ok)
                copy_bytes(out, recorded.text.data + from, bytes);
            arena_rewind(&text->data, ArenaMark{origin + written + bytes});
            written += bytes;
            message_started = true;
        }
    }
    if (overflow || written == 0) {
        arena_rewind(&text->data, ArenaMark{origin});
        if (overflowed)
            *overflowed = overflow;
        return {};
    }
    text->copy_mark = origin + 1;
    return Utf8View{reinterpret_cast<const char*>(text->data.base + origin), written};
}

} // namespace maicord
