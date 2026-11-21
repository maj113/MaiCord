#pragma once

#include "maicord/core/base.hpp"

namespace maicord::app_detail {

bool application_present_frame() noexcept;
void application_update_frame_rate() noexcept;
void application_present_pending_frame() noexcept;
void application_record_presentation_failure(const char* stage) noexcept;
void application_record_presentation_success() noexcept;
void application_resize_client() noexcept;
void application_resize_surface(u32 width, u32 height) noexcept;
// Shared visual step clock: loading spinners (pins, inbox, thread browser, the caption's connection indicator)
// and the composer's typing dots. Sampled once per presented frame; zero while
// neither is on screen.
u64 application_loading_spinner_wake() noexcept;
bool application_loading_spinner_due(u64 now_ms) noexcept;
// The spinners' phase of this frame (ui_spinner_phase).
u8 application_loading_spinner_phase() noexcept;

} // namespace maicord::app_detail
