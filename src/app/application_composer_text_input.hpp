#pragma once
#include "maicord/core/base.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/ui/geometry.hpp"

#include "maicord/core/win32_types.hpp"
namespace maicord::app_detail {
bool application_composer_text_input_synchronize() noexcept;
void application_composer_text_input_notify_layout() noexcept;
void application_composer_text_input_suspend() noexcept;
void application_composer_text_input_release() noexcept;
bool application_composer_text_input_filter(const MSG& message) noexcept;
bool application_composer_text_input_composing() noexcept;
// Client-DIP bounds of bytes [begin, end) of the edit that owns native text
// input, when its text equals text (UI Automation text ranges).
bool application_composer_text_input_range_bounds(Utf8View text, u32 begin, u32 end, UiRect* output) noexcept;
}
