#pragma once
#include "maicord/ui/shell/notice_band.hpp"

namespace maicord::app_detail {
// The app-wide notice band (ui/shell/notice_band.hpp): the highest-priority notice of its sources, in the official
// client's order, shows across the top of the app and the shell lays out below it.
//   Streamer Mode: "Streamer Mode is enabled." with Disable; the X hides it until Streamer Mode turns on again.
//   Application Test Mode: "Test mode is currently active for **{application}**. ..." on the warning tint; the X
//   ends test mode, as the official notice's.
enum class ApplicationNoticeSource : u8 { streamer_mode, test_mode, count };
// Re-reads the sources and, when the shown notice or its expansion changed, lays the shell out again. Cheap when
// nothing changed; the main loop calls it every frame.
void application_notice_synchronize(u64 now_ms) noexcept;
// The band to lay out (UiShellLayoutMetrics::notice_height, UiScene::notice): null and 0 without one.
const UiNoticeBand* application_notice_band() noexcept;
float application_notice_height() noexcept;
// A click at (x, y) content DIPs: the button runs its action, the X dismisses; true when the band took it.
bool application_notice_click(float x, float y) noexcept;
// The same parts by name (screen readers): false when the shown notice has no such part.
bool application_notice_activate(UiNoticeBandPart part) noexcept;
} // namespace maicord::app_detail
