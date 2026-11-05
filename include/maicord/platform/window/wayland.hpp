#pragma once

#include "maicord/platform/window/clipboard.hpp"
#include "maicord/platform/window/dropped_files.hpp"
#include "maicord/platform/window/window.hpp"

// Services of the Wayland window backend (Linux) that have no Windows counterpart: what the GPU backend needs to
// create an EGL window surface on the window's wl_surface, and bring-up presentation without a GPU backend.
// The portable window service is window.hpp; window_surface_native() returns the wl_surface*.

namespace maicord {

struct WaylandSurfaceInfo {
    void* display;  // wl_display*: eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display)
    void* surface;  // wl_surface*, the same as window_surface_native
    u32 width_px, height_px;  // the buffer size the window wants now: logical size at the scale
    u32 scale_120;            // 120 * dpi / 96, as WindowMetrics
    u32 logical_width, logical_height;  // the surface size in compositor units; the viewport destination
};
bool window_wayland_info(Window window, WaylandSurfaceInfo* info) noexcept;

// Connects to the compositor ahead of the window (window_create then finds the connection made) and returns the
// wl_display*, or null when there is no compositor. The GPU device is made on this display, and its start-up overlaps
// the creation of the window.
void* window_wayland_connect() noexcept;

// The GPU backend takes over the surface's buffers (true): from now on it attaches and commits (eglSwapBuffers) and
// the window stops presenting its wl_shm clear. The window keeps the wp_viewport destination at the logical size
// and sends frame callbacks through window_frame_callback_request. False hands the surface back; the window
// presents its clear again.
void window_wayland_use_external_buffers(Window window, bool external) noexcept;

// The colour (0xRRGGBB) of the wl_shm clear the window presents while it owns the buffers. Dark grey by default.
void window_wayland_set_clear_color(Window window, u32 xrgb) noexcept;

// The globals the compositor advertised, in registry order: false past the last. The name stays valid until the
// window is destroyed. What the protocol support matrix and diagnostics are built from.
bool window_wayland_global(u32 index, const char** name, u32* version) noexcept;

// Decorations. The window asks the compositor for client-side decorations (the app draws its own caption bar, D2); a
// compositor that insists on its own makes window_caps().csd false and the app hides the bar. True prefers the
// compositor's.
void window_wayland_prefer_server_decorations(Window window, bool server) noexcept;

// The window tells the compositor its whole surface is opaque (wl_surface.set_opaque_region, kept in step with the size and
// the scale): it has no rounded or transparent corners, and the GPU backend draws into an EGL config without alpha, so the
// compositor can skip blending and what lies behind the window. This reads the region last sent, in surface units (the
// size of WaylandSurfaceInfo::logical_*), for tests and diagnostics; false before the first one.
bool window_wayland_opaque_region(Window window, u32* width, u32* height) noexcept;

// Activation (xdg-activation-v1). window_request_attention(Attention::flash) does this; the call asks the compositor
// to focus the window, or mark it urgent when focus stealing is refused. Needs a recent input event for the token.
void window_wayland_request_activation(Window window) noexcept;
// Focuses the window with a token someone else made: the XDG_ACTIVATION_TOKEN of a second launch or the activation-token
// of a notification click (platform/dbus/instance.hpp, notifications.hpp). False without xdg-activation or a token.
bool window_wayland_activate_with_token(Window window, const char* token) noexcept;
// The activation tokens the compositor has answered, for diagnostics and tests.
u32 window_wayland_activation_count() noexcept;

// Idle inhibit (a call, a video): the screen saver and the idle timeout stay off while true. False when the compositor
// has no zwp_idle_inhibit_manager_v1.
bool window_wayland_set_idle_inhibit(Window window, bool inhibit) noexcept;

// A wl_egl_window on the window's surface for eglCreateWindowSurface, sized to the window and kept in step with its size
// and scale by the window; the GPU backend then presents with eglSwapBuffers. Implies window_wayland_use_external_buffers.
// Null when libwayland-egl is not installed. A GPU backend that creates its own wl_egl_window does not call these.
void* window_wayland_egl_window(Window window) noexcept;
void window_wayland_egl_release(Window window) noexcept;

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

// The primary selection (select-to-copy, middle-click to paste). Reading and writing are the clipboard's: a bounded read,
// and a write that needs a recent input event.
ClipboardTextResult primary_selection_read_utf8(const Slice<char>& output) noexcept;
bool primary_selection_write_utf8(const Utf8View& text) noexcept;
bool primary_selection_has_text() noexcept;

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
