#pragma once

#include "maicord/core/base.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {

bool application_present_frame() noexcept;
void application_update_frame_rate(u64 now_us) noexcept;
void application_present_pending_frame() noexcept;
void application_record_presentation_failure(const char* stage) noexcept;
void application_record_presentation_success() noexcept;
void application_resize_client(HWND window) noexcept;
void application_resize_surface(u32 width, u32 height) noexcept;
// Shared loading-spinner frame clock (pins, inbox, thread browser). Sampled
// once per presented frame; zero while no spinner is on screen.
u64 application_loading_spinner_wake() noexcept;
bool application_loading_spinner_due(u64 now_ms) noexcept;

} // namespace maicord::app_detail
