#pragma once

#include "maicord/core/base.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {

LRESULT CALLBACK application_window_proc(HWND window, UINT message, WPARAM wparam,
                                         LPARAM lparam) noexcept;

} // namespace maicord::app_detail
