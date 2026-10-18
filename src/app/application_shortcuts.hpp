#pragma once
#include "maicord/app/shortcut_registry.hpp"
#include "maicord/ui/input_router.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
// Window side of the shortcut registry: the focus context, dispatch of
// resolved actions, background voice keybinds and the Ctrl+/ overlay.
ShortcutContext application_shortcut_context() noexcept;
// First registry action for a key-down in the current context.
ShortcutAction application_shortcut_action(u32 key) noexcept;
// The registry's candidates in precedence order. Repeats move navigation and
// are swallowed by one-shot actions. True when handled.
bool handle_shortcut_key(u32 key, bool repeated) noexcept;
// Custom (voice) keybinds apply everywhere in the window, ahead of modal
// surfaces; they never share a built-in, text-editing or system chord.
bool application_custom_keybind_input(UiInputKind kind, u32 code, bool repeated) noexcept;

// Background voice keybinds: polls the custom chords (the push-to-talk
// mechanism) while a call is active and another window has focus.
void application_shortcuts_pump() noexcept;
u64 application_shortcuts_next_wake() noexcept;

// Keyboard shortcuts overlay.
bool application_shortcuts_overlay_active() noexcept;
bool application_shortcuts_overlay_open() noexcept;
void application_shortcuts_overlay_close() noexcept;
bool application_shortcuts_overlay_input(UiInputKind kind, u32 code, bool repeated) noexcept;
bool application_shortcuts_overlay_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
bool application_shortcuts_overlay_synchronize() noexcept;
void application_shortcuts_overlay_release() noexcept;
} // namespace maicord::app_detail
