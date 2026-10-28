#pragma once
#include "maicord/core/win32_process.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/ui/geometry.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
struct ApplicationQuickSwitcher;
bool application_quick_switcher_active() noexcept;
// Friend picker for a new conversation (zero) or for adding people to a DM or group.
bool application_quick_switcher_open_picker(u64 channel_id) noexcept;
void application_quick_switcher_close() noexcept;
void application_quick_switcher_release() noexcept;
bool application_quick_switcher_synchronize() noexcept;
bool application_quick_switcher_input(UiInputKind, u32 key, bool repeated) noexcept;
bool application_quick_switcher_filter(const MSG&) noexcept;
bool application_quick_switcher_window_message(UINT, WPARAM, LPARAM) noexcept;
void application_quick_switcher_note_channel(u64 channel_id) noexcept;
void application_quick_switcher_tick(u64 now_ms) noexcept;
u64 application_quick_switcher_caret_wake() noexcept;
MessageComposer* application_quick_switcher_edit_model() noexcept;
TextInputTarget application_quick_switcher_edit_target() noexcept;
UiRect application_quick_switcher_edit_bounds() noexcept;
bool application_quick_switcher_edit_focused() noexcept;
bool application_quick_switcher_edit_changed(bool content = true) noexcept;
void application_quick_switcher_edit_cancel_native_group(const TextInputTarget&) noexcept;
u32 application_quick_switcher_owner_bytes() noexcept;
} // namespace maicord::app_detail
