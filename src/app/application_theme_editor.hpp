#pragma once
#include "maicord/ui/theme_editor.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/ui/scroll_host.hpp"

#include "maicord/core/win32_types.hpp"
namespace maicord::app_detail {
struct ApplicationThemeEditor;
bool application_theme_editor_open() noexcept;
void application_theme_editor_close(bool commit = true) noexcept;
void application_theme_editor_release() noexcept;
bool application_theme_editor_active() noexcept;
bool application_theme_editor_synchronize() noexcept;
UiScrollBinding application_theme_editor_scroll_binding() noexcept;
// Called once by frame preparation after input has published its latest draft.
bool application_theme_editor_publish_foreground() noexcept;
bool application_theme_editor_input(UiInputKind, u32 code, bool repeated) noexcept;
bool application_theme_editor_window_message(UINT, WPARAM, LPARAM) noexcept;
const UiThemeEditorScene* application_theme_editor_scene() noexcept;
u32 application_theme_editor_owner_bytes() noexcept;
} // namespace maicord::app_detail
