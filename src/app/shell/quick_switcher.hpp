#pragma once
#include "maicord/model/chat/message_composer.hpp"
#include "maicord/ui/core/input_router.hpp"
#include "maicord/ui/core/geometry.hpp"

#include "maicord/platform/window/event.hpp"

namespace maicord::app_detail {
bool application_quick_switcher_active() noexcept;
// Opens the switcher (Ctrl+K; Home's "Find or start a conversation").
bool application_quick_switcher_open() noexcept;
// Friend picker for a new conversation (zero) or for adding people to a DM or group.
bool application_quick_switcher_open_picker(u64 channel_id) noexcept;
void application_quick_switcher_close() noexcept;
void application_quick_switcher_release() noexcept;
bool application_quick_switcher_synchronize() noexcept;
bool application_quick_switcher_input(UiInputKind, u32 key, bool repeated) noexcept;
bool application_quick_switcher_filter(const WindowEvent&) noexcept;
bool application_quick_switcher_window_event(const WindowEvent&) noexcept;
void application_quick_switcher_note_channel(u64 channel_id) noexcept;
void application_quick_switcher_tick(u64 now_ms) noexcept;
u64 application_quick_switcher_caret_wake() noexcept;
MessageComposer* application_quick_switcher_edit_model() noexcept;
TextInputTarget application_quick_switcher_edit_target() noexcept;
UiRect application_quick_switcher_edit_bounds() noexcept;
bool application_quick_switcher_edit_focused() noexcept;
bool application_quick_switcher_edit_changed() noexcept;
void application_quick_switcher_edit_cancel_native_group(const TextInputTarget&) noexcept;
} // namespace maicord::app_detail
