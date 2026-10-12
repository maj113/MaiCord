#pragma once

namespace maicord {

struct UiRect;
struct Utf8View;

namespace app_detail {

UiRect application_viewport_dips() noexcept;
Utf8View application_account_notice() noexcept;
bool synchronize_navigation_view() noexcept;
bool synchronize_shell_widget_scenes() noexcept;
bool synchronize_shell_layout() noexcept;
bool rebuild_account_text_cache() noexcept;
bool refresh_account_text(bool force = false) noexcept;
bool refresh_account_scroll() noexcept;
bool refresh_account_frame(bool force = false) noexcept;

} // namespace app_detail
} // namespace maicord
