#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {

// Shared multi-click pointer selection for every text surface. Windows decides
// the double click (WM_LBUTTONDBLCLK, from the system double-click time and
// rectangle); the third click arrives as an ordinary press and is recognized
// here with the same limits. Counts are 1, 2 or 3; a third click needs a real
// double click before it, so a plain press never escalates.
struct UiClickPolicy {
    u32 interval_ms; // GetDoubleClickTime()
    float slop_x;    // half of SM_CXDOUBLECLK, in the caller's coordinate space
    float slop_y;    // half of SM_CYDOUBLECLK
};

struct UiClickTracker {
    const void* owner;
    u32 time_ms;
    float x;
    float y;
    u8 count;
};

// `time_ms` is the message time (GetMessageTime); wrap-around safe.
// `system_double_click` is true for WM_LBUTTONDBLCLK.
inline u8 ui_click_tracker_press(UiClickTracker* tracker, const void* owner, float x, float y, u32 time_ms,
                                 UiClickPolicy policy, bool system_double_click) noexcept {
    const float dx = x > tracker->x ? x - tracker->x : tracker->x - x;
    const float dy = y > tracker->y ? y - tracker->y : tracker->y - y;
    const bool triple = !system_double_click && tracker->count == 2 && tracker->owner == owner &&
                        static_cast<u32>(time_ms - tracker->time_ms) <= policy.interval_ms &&
                        dx <= policy.slop_x && dy <= policy.slop_y;
    tracker->count = system_double_click ? u8{2} : triple ? u8{3} : u8{1};
    tracker->owner = owner;
    tracker->time_ms = time_ms;
    tracker->x = x;
    tracker->y = y;
    return tracker->count;
}

// Keyboard input, focus loss and presses elsewhere end a click sequence.
inline void ui_click_tracker_reset(UiClickTracker* tracker) noexcept {
    reset_value<UiClickTracker>(tracker);
}

enum class UiTextSelectionUnit : u8 { character, word, line };

constexpr UiTextSelectionUnit ui_text_selection_unit(u8 clicks) noexcept {
    return clicks >= 3 ? UiTextSelectionUnit::line
           : clicks == 2 ? UiTextSelectionUnit::word
                         : UiTextSelectionUnit::character;
}

// The unit selected by the press; drags extend by whole units from it.
struct UiTextPointerSelection {
    u32 origin_first;
    u32 origin_last;
    UiTextSelectionUnit unit;
};

// Range of `unit` at a pointer position. Single-line fields select all text
// on a triple click; multi-line text selects the line/paragraph.
inline bool ui_text_unit_range(Utf8View text, u32 position, UiTextSelectionUnit unit, bool single_line,
                               u32* first, u32* last) noexcept {
    if (position > text.bytes)
        return false;
    if (unit == UiTextSelectionUnit::word)
        return utf8_word_range_near(text, position, first, last);
    if (unit == UiTextSelectionUnit::line) {
        if (!single_line)
            return utf8_line_range(text, position, first, last);
        *first = 0;
        *last = text.bytes;
        return true;
    }
    *first = *last = position;
    return true;
}

// A press. Returns false when the selection must not change: invalid input, or
// an active IME composition (the composition string, its attributes and the
// candidate window stay anchored; TSF owns the selection until it ends).
inline bool ui_text_pointer_press(Utf8View text, u32 position, u8 clicks, bool single_line, bool shift,
                                  u32 current_anchor, bool composing, UiTextPointerSelection* state,
                                  u32* anchor, u32* caret) noexcept {
    if (composing || position > text.bytes || current_anchor > text.bytes)
        return false;
    const auto unit = ui_text_selection_unit(clicks);
    if (unit == UiTextSelectionUnit::character) {
        const u32 origin = shift ? current_anchor : position;
        *state = {origin, origin, unit};
        *anchor = origin;
        *caret = position;
        return true;
    }
    u32 first = 0, last = 0;
    if (!ui_text_unit_range(text, position, unit, single_line, &first, &last))
        return false;
    *state = {first, last, unit};
    *anchor = first;
    *caret = last;
    return true;
}

// A drag after a press. Word/line selections grow by whole units and always
// keep the originally selected unit; they never shrink inside it.
inline bool ui_text_pointer_drag(Utf8View text, const UiTextPointerSelection& state, u32 position,
                                 bool single_line, bool composing, u32* anchor, u32* caret) noexcept {
    if (composing || position > text.bytes || state.origin_first > state.origin_last ||
        state.origin_last > text.bytes)
        return false;
    if (state.unit == UiTextSelectionUnit::character) {
        *anchor = state.origin_first;
        *caret = position;
        return true;
    }
    if (position >= state.origin_first && position <= state.origin_last) {
        *anchor = state.origin_first;
        *caret = state.origin_last;
        return true;
    }
    u32 first = 0, last = 0;
    const bool ranged = state.unit == UiTextSelectionUnit::word
                            ? utf8_word_range(text, position, &first, &last)
                            : ui_text_unit_range(text, position, state.unit, single_line, &first, &last);
    if (!ranged)
        return false;
    if (position < state.origin_first) {
        *anchor = state.origin_last;
        *caret = first < state.origin_first ? first : state.origin_first;
    } else {
        *anchor = state.origin_first;
        *caret = last > state.origin_last ? last : state.origin_last;
    }
    return true;
}

} // namespace maicord
