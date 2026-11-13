#pragma once

#include "maicord/platform/window/clipboard.hpp"
#include "maicord/platform/window/dropped_files.hpp"
#include "maicord/platform/window/window.hpp"

// Services of the Wayland window backend (Linux) that have no Windows counterpart: what the GPU backend needs to
// create an EGL window surface on the window's wl_surface, and the desktop integration the compositor's protocols give.
// The portable window service is window.hpp; window_surface_native() returns the wl_surface*.

namespace maicord {

// Connects to the compositor ahead of the window (window_create then finds the connection made) and returns the
// wl_display*, or null when there is no compositor. The GPU device is made on this display, and its start-up overlaps
// the creation of the window.
void* window_wayland_connect() noexcept;

// Activation (xdg-activation-v1). window_request_attention(Attention::flash) asks the compositor to focus the window,
// or mark it urgent when focus stealing is refused; it needs a recent input event for the token.
// Focuses the window with a token someone else made: the XDG_ACTIVATION_TOKEN of a second launch or the activation-token
// of a notification click (platform/dbus/instance.hpp, notifications.hpp). False without a window, xdg-activation or a token.
bool window_wayland_activate(const char* token) noexcept;

// Idle inhibit (a call, a video): the screen saver and the idle timeout stay off while true. False when the compositor
// has no zwp_idle_inhibit_manager_v1.
bool window_wayland_set_idle_inhibit(Window window, bool inhibit) noexcept;

// The system's input idle time (ext-idle-notify-v1; the notification that ignores idle inhibitors where the compositor
// has version 2). Starts watching for `timeout_ms` without input; from then on PlatformSettings::system_idle_ms is 0 while
// the user is active and `timeout_ms` plus the time since the compositor said so once idle (the protocol only says
// "idle for at least the timeout"). Without the protocol (GNOME's Mutter has none) it stays kSystemIdleUnknown and the
// call returns false. Calling again replaces the watch.
bool window_wayland_watch_idle(Window window, u32 timeout_ms) noexcept;

// Rings the compositor's system bell for the window (xdg-system-bell-v1); false where it has none.
bool window_wayland_system_bell() noexcept;

// Text input (IME, zwp_text_input_v3). The app says a text field has focus, with the text around the caret and where the
// caret is; the input method answers with ime_preedit, ime_commit and ime_delete_surrounding events (UTF-16, delete_*
// in UTF-16 units). Without the protocol the call returns false and the keyboard's own text is all there is.
enum class TextInputPurpose : u8 { normal, digits, number, phone, url, email, password, pin };
struct TextInputRequest {
    Utf8View surrounding;  // UTF-8 text around the caret; cut at most 4000 bytes at character boundaries; unused when sensitive
    u32 cursor_byte, anchor_byte;  // caret and selection anchor in `surrounding`
    TextInputPurpose purpose;
    bool sensitive;  // a password or secret: hidden text, no learning, no surrounding text
    i32 x, y, width, height;  // the caret's rectangle in client pixels
};
// The window is named by its surface (window_surface_native), as the text store holds it.
bool window_wayland_text_input_available(NativeWindow window) noexcept;
// Enables or updates text input for the focused field; applied at once, or when the window gets text focus.
bool window_wayland_text_input_enable(NativeWindow window, const TextInputRequest& request) noexcept;
void window_wayland_text_input_disable(NativeWindow window) noexcept;

// What clipboard_capture_attachment found: files (their paths, as dropped files) or a PNG image.
enum class ClipboardCaptureKind : u8 { files, image_png };
ClipboardCaptureKind clipboard_capture_kind(const ClipboardCapture* capture) noexcept;
// Valid until the capture is released; the files are not to be released separately.
const DroppedFiles* clipboard_capture_files(const ClipboardCapture* capture) noexcept;
ConstSlice<u8> clipboard_capture_png(const ClipboardCapture* capture) noexcept;

// Push-to-talk beyond the focused window: binds `key` with `modifiers` (Modifier bits) as a GlobalShortcuts portal shortcut
// ("Push to talk" in the compositor's settings; the key is the preferred trigger the user may change). While the portal
// holds it down, global_key_down(key) is true even when the window is not focused. Replaces an earlier binding; false
// without a session bus. Where the portal does not exist the key stays focused-window only: _ready() says which.
bool window_wayland_global_shortcut(Key key, u8 modifiers, const char* description) noexcept;
bool window_wayland_global_shortcut_ready() noexcept;
void window_wayland_global_shortcut_clear() noexcept;

} // namespace maicord
