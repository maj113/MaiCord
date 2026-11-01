#pragma once
#include "maicord/core/base.hpp"
namespace maicord::app_detail {
bool handle_content_focus_key(u32 key) noexcept;
bool handle_thread_header_key(u32 key, bool down, bool repeated = false) noexcept;
} // namespace maicord::app_detail
