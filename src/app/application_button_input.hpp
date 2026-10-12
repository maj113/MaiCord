#pragma once
#include "maicord/core/base.hpp"

#include "maicord/core/win32_types.hpp"
namespace maicord::app_detail {
bool application_button_pointer(UINT message, LPARAM lparam) noexcept;
}
