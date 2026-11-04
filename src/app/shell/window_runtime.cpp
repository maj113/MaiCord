// The Windows-only half of the window procedure (win32.hpp's hooks): the window state observer, the caption frame, UI
// Automation and the shell integration. The portable event chain is window_dispatch.cpp.
#include "../account/application_account_logout.hpp"
#include "../effects/effects.hpp"
#include "../notifications/taskbar_attention.hpp"
#include "../accessibility/accessibility.hpp"
#include "../notifications/desktop.hpp"
#include "state.hpp"
#include "caption.hpp"
#include "overlay_input.hpp"
#include "window_events.hpp"
#include "window_runtime.hpp"
#include "window_state.hpp"
#include "maicord/sys/shell.hpp"

namespace maicord::app_detail {
namespace {
// The window procedure is the only place window state changes are observed. Position changes carry every
// show/hide/minimize/restore/resize; activation, focus and DPI have their own messages. The backend runs this first,
// before any other hook or handler reads the flags.
void observe_window_state(void*, const WindowEvent& event) noexcept {
    switch (event.type) {
    case EventType::key_down:
    case EventType::key_up:
    case EventType::character:
    case EventType::pointer_move:
    case EventType::pointer_down:
    case EventType::pointer_up:
    case EventType::wheel:
        application_effects_note_input();
        break;
    case EventType::state_changed:
        application_window_observe_position(static_cast<HWND>(event.window));
        break;
    case EventType::activation_changed:
        g_window_state.foreground = event.focus.gained;
        break;
    case EventType::focus_changed:
        g_window_state.keyboard_focus = event.focus.gained;
        break;
    case EventType::scale_changed:
        g_window_state.dpi = input_dpi(event);
        break;
    default:
        break;
    }
}

bool input_blocked(void*) noexcept {
    return application_input_blocked();
}

// The caption owns the frame and the rows above the content.
bool frame_hook(void*, HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) noexcept {
    return application_caption_window_message(window, message, wparam, lparam, result);
}

// UI Automation (WM_GETOBJECT) and its deferred actions (it observes WM_DESTROY); tray icon callbacks, Explorer
// restarts and launches forwarded by a second instance; the taskbar button's registered "created" message.
bool shell_hook(void*, HWND, UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) noexcept {
    if (application_accessibility_window_message(message, wparam, lparam, result))
        return true;
    if (application_desktop_window_message(message, wparam, lparam, result))
        return true;
    if (application_input_blocked())
        return false;
    // A press on the frame, or a touch or pen contact, hides keyboard focus as a click in the client does.
    if ((message == WM_NCLBUTTONDOWN || message == WM_POINTERDOWN) && g_app.keyboard_focus_visible) {
        g_app.keyboard_focus_visible = false;
        application_request_frame();
    }
    if (application_taskbar_attention_window_message(message)) {
        *result = 0;
        return true;
    }
    return false;
}

} // namespace

const Win32Hooks* application_window_hooks() noexcept {
    static const Win32Hooks hooks{nullptr, observe_window_state, frame_hook, shell_hook, input_blocked};
    return &hooks;
}

} // namespace maicord::app_detail
