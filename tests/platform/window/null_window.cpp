#include "maicord/platform/window/null.hpp"
#include "maicord/platform/window/event.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/platform/window/window.hpp"
#include "maicord/ui/core/cursor.hpp"
#include "../../support/test_output.hpp"

// The headless window backend (src/platform/window/null) behaves as the system does where handlers depend on it:
// capture notification timing as captured from a real window in win32_behaviour.cpp, key and pointer state held in
// memory, events injected through the sink.
namespace maicord {
namespace {

u32 failures;

void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

struct Seen {
    u32 events;
    u32 capture_lost;
    u32 capture_lost_self;
    u32 synthetic;
    EventType last;
    NativeWindow other;
};
Seen g_seen;
Window g_window;
bool g_handle;

void sink(void*, const WindowEvent& event, WindowEventResult* result) noexcept {
    ++g_seen.events;
    g_seen.last = event.type;
    if (event.type == EventType::capture_lost) {
        ++g_seen.capture_lost;
        g_seen.other = event.focus.other;
        if (event.focus.other == g_window)
            ++g_seen.capture_lost_self;
    }
    if (event.flags & ev_synthetic)
        ++g_seen.synthetic;
    result->handled = g_handle;
}

void capture() noexcept {
    // The first take is silent; a held capture released tells the sink once, before the call returns.
    check(!window_pointer_captured(g_window), "null window: nothing is captured at the start");
    window_capture_pointer(g_window);
    check(window_pointer_captured(g_window) && g_seen.capture_lost == 0 && null_window_probe().captured &&
              null_window_probe().captures == 1,
          "null window: the first capture is silent");

    window_capture_pointer(g_window);
    check(g_seen.capture_lost == 1 && g_seen.capture_lost_self == 1 && window_pointer_captured(g_window),
          "null window: capturing again names the window itself in a synchronous capture_lost");

    g_seen.capture_lost = g_seen.capture_lost_self = 0;
    window_release_capture(g_window);
    check(!window_pointer_captured(g_window) && g_seen.capture_lost == 1 && g_seen.last == EventType::capture_lost &&
              g_seen.other == nullptr,
          "null window: releasing the capture sends one capture_lost naming nobody, before the call returns");

    g_seen.capture_lost = 0;
    window_release_capture(g_window);
    check(g_seen.capture_lost == 0, "null window: releasing with no capture sends nothing");
    check(null_window_probe().releases == 1, "null window: the probe counts only the release that held the capture");
}

void keys() noexcept {
    null_window_release_keys();
    check(window_modifiers(nullptr) == 0 && !window_key_down(nullptr, key::ctrl) &&
              !window_toggle_state(nullptr, key::caps_lock),
          "null window: a fresh state reads as nothing held");
    null_window_set_key(key::ctrl, true);
    null_window_set_key(key::shift, true);
    check(window_key_down(g_window, key::ctrl) && window_modifiers(nullptr) == (mod_ctrl | mod_shift) &&
              !window_key_down(nullptr, key::alt),
          "null window: held keys read through the service, with or without the window");
    null_window_set_toggle(key::caps_lock, true);
    check(window_toggle_state(nullptr, key::caps_lock) && (window_modifiers(nullptr) & mod_caps) != 0,
          "null window: a lock key reads as toggled and as a modifier bit");
    null_window_release_keys();
    check(window_key_down(nullptr, key::ctrl) == false && (window_modifiers(nullptr) & (mod_ctrl | mod_shift)) == 0 &&
              window_toggle_state(nullptr, key::caps_lock),
          "null window: releasing the keys lets go of the held keys but not of a toggled lock");
    null_window_set_toggle(key::caps_lock, false);
    check(window_modifiers(nullptr) == 0, "null window: the lock toggles back off");
    null_window_set_layout_types_text(false);
    check(!window_layout_types_text(nullptr, 'A', 0), "null window: the layout answer is what the test set");
    null_window_set_layout_types_text(true);
    check(window_layout_types_text(nullptr, 'A', 0), "null window: and can be set the other way");
}

void state() noexcept {
    float x = 1, y = 1;
    null_window_set_pointer(12.5f, 40.f);
    check(window_pointer_position(g_window, &x, &y) && x == 12.5f && y == 40.f,
          "null window: the pointer is where the test put it");

    window_set_cursor(g_window, UiCursor::hand);
    window_set_cursor(g_window, UiCursor::ibeam);
    check(null_window_probe().cursor == static_cast<u8>(UiCursor::ibeam) && null_window_probe().cursor_changes == 2,
          "null window: the cursor shape and the number of changes are recorded");
    window_request_cursor_refresh(g_window);
    check(null_window_probe().cursor_refreshes == 1, "null window: cursor refresh requests are counted");

    null_window_set_focus_allowed(false);
    check(!window_focus(g_window) && !null_window_probe().focused, "null window: focus can be refused");
    null_window_set_focus_allowed(true);
    check(window_focus(g_window) && null_window_probe().focused && null_window_probe().focus_requests == 2,
          "null window: focus is taken when allowed");

    window_set_title(g_window, utf8_literal("headless"));
    check(null_window_probe().title_bytes == 8 && null_window_probe().title[0] == 'h', "null window: the title is kept");
    window_invalidate(g_window);
    window_wake(g_window);
    window_request_close(g_window);
    check(null_window_probe().invalidations == 1 && null_window_probe().wakes == 1 &&
              null_window_probe().close_requests == 1,
          "null window: invalidations, wakes and close requests are counted");
}

void inject() noexcept {
    g_seen = {};
    WindowEvent event{};
    event.type = EventType::pointer_move;
    event.pointer.x = 3;
    event.pointer.y = 4;
    WindowEventResult result{};
    g_handle = false;
    check(window_inject(g_window, event, &result) && g_seen.events == 1 && g_seen.last == EventType::pointer_move &&
              g_seen.synthetic == 1 && !result.handled,
          "null window: an injected event reaches the sink marked synthetic and keeps the sink's answer");
    g_handle = true;
    check(window_inject(g_window, event, &result) && result.handled,
          "null window: a handled injected event answers handled");
    g_handle = false;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_window_null_entry) {
    using namespace maicord;
    null_window_reset();
    g_seen = {};
    g_window = {};
    check(window_create({}, sink, nullptr, &g_window) && g_window && null_window_probe().created,
          "null window: the headless window opens");
    if (g_window) {
        capture();
        keys();
        state();
        inject();
        window_destroy(g_window);
        check(!null_window_probe().created, "null window: destroying it clears the probe");
    }
    test_exit(failures, "window null");
}
