#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/core/geometry.hpp"

namespace maicord {

struct MessageRichStore;
struct MessageScene;
struct MessageStore;
struct MessageTextCommand;
struct MessageTextPresentation;
struct UiTextPointerSelection;

// A text selection over the message timeline, as a browser selection over the official client's message list: from
// an anchor to a focus, each a byte offset in the text of one command of a message, and every command between them.
// A point names its command by what survives rebuilds, scrolling and virtualization (never a row or a command
// index): the message and its revision, the command's text source and kind, and which of the message's commands
// with that source and kind it is. Messages order by id, as the timeline does. A point whose message is gone or
// changed is a boundary at that message: the selection keeps the messages beyond it and drops it.
struct MessageTextSelectionPoint {
    u64 message_id;
    u32 message_revision;
    u32 byte_offset;
    u16 source_id;
    u16 nth;
    u8 kind; // MessageTextMeasurementKind
    u8 slot;
    u16 reserved;
};

struct MessageTextSelectionRange {
    MessageTextSelectionPoint anchor;
    MessageTextSelectionPoint focus;
    // Changes with either point; zero for no selection.
    u64 signature;
};

// The selection in one presentation: commands first..last in command order, the first from first_offset, the last
// up to last_offset (exclusive), those between whole. Commands outside it, and unselectable ones inside, show none.
struct MessageTextSelectionSpan {
    u32 first;
    u32 first_offset;
    u32 last;
    u32 last_offset;
    bool valid;
};

inline constexpr u32 kMessageTextSelectionNoCommand = ~0u;
// Bounds on a selection: it spans at most this many messages of the timeline (a drag past them selects no further),
// and its kept text and copy share kMessageTextSelectionTextBytes, half for each.
inline constexpr u32 kMessageTextSelectionMaximumMessages = 10'000;
inline constexpr usize kMessageTextSelectionTextBytes = usize{64} << 20;

// Text a selection takes part in: a message's own text, never reactions, the unread divider or the intro.
bool message_text_command_selectable(const MessageTextCommand& command) noexcept;

inline bool message_text_selection_active(const MessageTextSelectionRange& range) noexcept {
    return range.signature != 0;
}
// A selection that holds text (its ends differ).
bool message_text_selection_nonempty(const MessageTextSelectionRange& range) noexcept;

// The point at a byte offset of a presented command; false for a command no selection takes part in.
bool message_text_selection_point(const MessageTextPresentation& presentation, u32 command_index, u32 byte_offset,
                                  MessageTextSelectionPoint* point) noexcept;
// The command a point names, or kMessageTextSelectionNoCommand when its message is not presented or changed.
u32 message_text_selection_find(const MessageTextPresentation& presentation,
                                const MessageTextSelectionPoint& point) noexcept;

// A press at a command's byte offset starts a selection there: one click collapsed at the offset, a double click the
// word, a triple click the paragraph, which drags then extend by whole units (`unit`).
bool message_text_selection_begin(MessageTextSelectionRange* range, UiTextPointerSelection* unit,
                                  const MessageTextPresentation& presentation, u32 command_index, u32 byte_offset,
                                  u8 clicks) noexcept;
// A drag to a command's byte offset moves the focus there, in this or any other message. Inside the anchor's command
// word and paragraph selections grow by whole units; beyond it the anchor holds the far edge of its unit. A focus
// that would span more than kMessageTextSelectionMaximumMessages messages of `store` is refused. Returns whether the
// selection changed.
bool message_text_selection_extend(MessageTextSelectionRange* range, const UiTextPointerSelection& unit,
                                   const MessageTextPresentation& presentation, const MessageStore& store,
                                   u32 command_index, u32 byte_offset) noexcept;
// The selection resolved against a presentation: linear in its commands, independent of the history.
MessageTextSelectionSpan message_text_selection_span(const MessageTextSelectionRange& range,
                                                     const MessageTextPresentation& presentation) noexcept;
// How far a drag held at `y` outside the message viewport scrolls in `elapsed_ms` (DIPs, negative upwards): faster
// the farther the pointer is past the edge. Zero inside it.
float message_text_selection_autoscroll(const UiRect& viewport, float y, u64 elapsed_ms) noexcept;

// The text of the selected messages as they were presented, kept for the copy while they scroll out of the
// presentation. Only messages inside the selection are kept, each once per presentation of it; the storage is
// reserved by the first selection and released with it, never per frame.
struct MessageTextSelectionCommand;
struct MessageTextSelectionMessage {
    u64 message_id;
    const MessageTextSelectionCommand* commands; // as presented; a change re-records the message
    u32 message_revision;
    u32 command_count;
};
struct MessageTextSelectionText {
    Arena messages; // MessageTextSelectionMessage, sorted by message id
    Arena data;     // commands, their text and semantics, and the copy's output
    u64 noted_signature;
    usize copy_mark; // one past where the last copy's output starts; zero for none
    // Bytes of kept text and copy output together; zero is kMessageTextSelectionTextBytes. Kept by a release.
    usize capacity;
    u32 noted_builds;
    u32 count;
    // A selected message's text did not fit at the last note: the copy refuses rather than copy less.
    bool incomplete;
};

// Records the selected messages a presentation shows. Cheap when neither the presentation nor the selection changed
// since the last call. False when a selected message's text did not fit (or storage could not be reserved): the
// caller keeps the selection it had, which fit.
bool message_text_selection_note(MessageTextSelectionText* text, const MessageTextSelectionRange& range,
                                 const MessageTextPresentation& presentation) noexcept;
void message_text_selection_text_release(MessageTextSelectionText* text) noexcept;

// The selection as text: every selected message in timeline order, its commands as presented joined by a newline (a
// space between commands side by side on one line), the messages by newlines. The ends take their part of their
// command; "(edited)" labels stay out and custom emoji and link chips copy their source, as a single message's copy
// does. A message inside the selection that was never presented while selected (a jump scrolled past it) copies its
// stored text unless its row is hidden. The text lives in `text` until the next note or copy; empty for nothing.
// Never a part: when the selection's text does not fit, it is empty and `overflow` is set.
Utf8View message_text_selection_copy(MessageTextSelectionText* text, const MessageTextSelectionRange& range,
                                     const MessageStore& store, const MessageRichStore* rich_store,
                                     const MessageScene* scene, bool* overflow) noexcept;

static_assert(sizeof(MessageTextSelectionPoint) == 24);
static_assert(sizeof(MessageTextSelectionRange) == 56);

} // namespace maicord
