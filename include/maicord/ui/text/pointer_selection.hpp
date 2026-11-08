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
u8 ui_click_tracker_press(UiClickTracker* tracker, const void* owner, float x, float y, u32 time_ms,
                          UiClickPolicy policy, bool system_double_click) noexcept;

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

// A press. Returns false when the selection must not change: invalid input, or
// an active IME composition (the composition string, its attributes and the
// candidate window stay anchored; TSF owns the selection until it ends).
bool ui_text_pointer_press(const Utf8View& text, u32 position, u8 clicks, bool single_line, bool shift,
                           u32 current_anchor, bool composing, UiTextPointerSelection* state,
                           u32* anchor, u32* caret) noexcept;

// A drag after a press. Word/line selections grow by whole units and always
// keep the originally selected unit; they never shrink inside it.
bool ui_text_pointer_drag(const Utf8View& text, const UiTextPointerSelection& state, u32 position,
                          bool single_line, bool composing, u32* anchor, u32* caret) noexcept;

} // namespace maicord
