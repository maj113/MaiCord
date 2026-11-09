#pragma once

// The headless window backend (src/platform/window/null): the whole window service over plain memory, with no
// desktop and no compositor, for the tests that drive window handlers (tests/support/window_fixture.hpp). It keeps
// what the system keeps: capture, focus, the cursor shape, key state, the pointer position; and, like the system,
// reports a lost capture to the sink synchronously, from inside the call that lost it
// (tests/platform/window/win32_behaviour.cpp captures the Windows behaviour it copies).
#include "maicord/platform/window/window.hpp"

namespace maicord {

// What the window service was asked to do since null_window_reset().
struct NullWindowProbe {
    bool created;
    bool captured;        // the window holds the pointer capture
    bool focused;         // the window holds the keyboard focus
    bool fullscreen;
    bool drop_target;     // window_set_drop_target(true) is in effect
    bool capture_excluded; // window_set_capture_excluded(true) is in effect
    bool shown;           // a window_show other than hide is in effect
    u8 cursor;            // the UiCursor of the last window_set_cursor
    u32 cursor_changes;   // window_set_cursor calls
    u32 cursor_refreshes; // window_request_cursor_refresh calls
    u32 captures;         // window_capture_pointer calls
    u32 releases;         // window_release_capture calls that held the capture
    u32 focus_requests;   // window_focus calls
    u32 wakes;            // window_wake calls
    u32 posts;            // window_post calls
    u32 close_requests;   // window_request_close calls
    u32 invalidations;    // window_invalidate calls
    u32 min_width, min_height;  // the last window_set_min_size
    u32 min_size_calls;         // window_set_min_size calls
    // window_wayland_global_shortcut*: the portal binding of the push-to-talk key.
    u8 shortcut_key;            // the key of the last bind; zero after a clear
    u32 shortcut_binds, shortcut_clears;
    u32 title_bytes;
    char title[128];      // the last window_set_title, truncated
    // window_wayland_text_input_*: the input method the text store talks to.
    bool text_input_available;  // set with null_window_set_text_input_available; what the call reports
    bool text_input_enabled;
    u32 text_input_enables;
    u32 text_surrounding_bytes;
    char text_surrounding[256];  // the surrounding text of the last request, truncated
    u32 text_cursor_byte, text_anchor_byte;
    u8 text_purpose;             // the TextInputPurpose
    bool text_sensitive;
    i32 text_x, text_y, text_height;
};
const NullWindowProbe& null_window_probe() noexcept;

// Forgets the window, the sink and everything the probe counted; key and pointer state go back to nothing held.
void null_window_reset() noexcept;

// The input device state window_modifiers, window_key_down, window_toggle_state, window_pointer_position and
// window_layout_types_text read.
void null_window_set_key(Key key, bool down) noexcept;
void null_window_set_toggle(Key key, bool on) noexcept;
void null_window_release_keys() noexcept;
void null_window_set_pointer(float x_px, float y_px) noexcept;
void null_window_set_metrics(const WindowMetrics& metrics) noexcept;
void null_window_set_layout_types_text(bool types_text) noexcept;
// Whether the window has a text input protocol at all (the Wayland compositor's text-input-v3).
void null_window_set_text_input_available(bool available) noexcept;

// Gives window_focus the outcome a real window would: whether it can take the keyboard focus at all.
void null_window_set_focus_allowed(bool allowed) noexcept;
// The keyboard focus moves to or away from the window by something else (the user, a dialog).
void null_window_set_focus(bool focused) noexcept;

} // namespace maicord
