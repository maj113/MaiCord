#pragma once
#include "maicord/core/base.hpp"

namespace maicord::app_detail {
// Video duration badges for composer upload tiles. Each MP4/MOV draft (a
// local file, or pasted file contents retained for the probe) is probed once
// on the system thread pool (movie header only); the UI pump collects results
// and retires probes whose drafts were removed or sent.
// Returns true when a new duration became available.
bool application_attachment_probe_synchronize() noexcept;
u32 application_attachment_probe_duration(u64 draft_id) noexcept;
// Waits for in-flight probes; called before attachment storage is released.
void application_attachment_probe_release() noexcept;
} // namespace maicord::app_detail
