#pragma once

#include "maicord/core/utf.hpp"

namespace maicord::app_detail {

bool application_pump_account() noexcept;
bool application_pump_visible_static_media(bool* repaint) noexcept;
// Deadline of the pending coalesced presence repaint, or zero.
u64 application_presence_next_wake() noexcept;
// Non-blocking composer notice while skipped gateway events may have left
// account state stale (empty when healthy).
Utf8View application_account_sync_notice() noexcept;

} // namespace maicord::app_detail
