#pragma once

#include "maicord/core/base.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {

void application_apply_minimum_window_track(HWND window, MINMAXINFO* limits) noexcept;
bool application_create_main_window(WNDPROC window_proc) noexcept;

} // namespace maicord::app_detail
