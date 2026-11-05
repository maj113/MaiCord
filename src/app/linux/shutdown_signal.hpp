#pragma once

// The Linux half of application_platform_quit_signal / application_platform_quit_requested (shell/window.hpp).
namespace maicord::app_detail {

// Blocks SIGTERM, SIGINT and SIGHUP for the process and opens the descriptor they are read from. Call once at the
// start of the process, before any thread exists (a thread started earlier keeps the default action).
void application_shutdown_signals_install() noexcept;

} // namespace maicord::app_detail
