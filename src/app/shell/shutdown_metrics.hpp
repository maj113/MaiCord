#pragma once

namespace maicord::app_detail {

// Writes the open message drafts at exit.
void application_persist_drafts_on_exit() noexcept;

#if defined(MAICORD_STARTUP_PROBE)
// Requires the explicit benchmark flag and an isolated runner profile marker.
// Call at entry before any desktop/account/profile initialization; false means
// the instrumented candidate must exit without starting the application.
bool application_startup_probe_initialize() noexcept;
// Records one successful Present return and requests normal window shutdown.
void application_startup_probe_first_present() noexcept;
// Main thread: keeps the first time of each named point (a static string)
// for the first-present report.
void application_startup_probe_mark(const char* name) noexcept;
#endif

} // namespace maicord::app_detail
