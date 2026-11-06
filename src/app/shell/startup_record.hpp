#pragma once
#include "maicord/core/base.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/platform/window/window.hpp"

namespace maicord::app_detail {
// The start of a session in the log, all under "[start]": a "==== MaiCord <version> start ====" line, then the build,
// the system and the machine; the display line once the window exists, the GPU line once the device does
// (startup_gpu.cpp) and the install line once the updater has read its channel. Versions, models, counts and the
// locale only: no user names, paths, tokens or account ids.
void application_log_start_record() noexcept;
void application_log_start_display(Window window) noexcept;
void application_log_start_gpu(GpuDevice* device) noexcept;
// After application_updates_initialize: how this copy was installed, its update channel and whether it updates itself.
void application_log_start_install() noexcept;
} // namespace maicord::app_detail
