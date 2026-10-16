#pragma once

namespace maicord::app_detail {

void application_log_shutdown_metrics() noexcept;

#if defined(MAICORD_STARTUP_PROBE)
// Requires the explicit benchmark flag and an isolated runner profile marker.
// Call at entry before any desktop/account/profile initialization; false means
// the instrumented candidate must exit without starting the application.
bool application_startup_probe_initialize() noexcept;
// Records one successful Present return and requests normal window shutdown.
void application_startup_probe_first_present() noexcept;
#endif

} // namespace maicord::app_detail
