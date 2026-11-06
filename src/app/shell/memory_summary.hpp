#pragma once
#include "maicord/core/base.hpp"

namespace maicord::app_detail {
// One info line, "[memory] summary reason=<reason> arenas=N committed_kib=N reserved_kib=N top=tag:kib/peak_kib,...
// gpu_atlas_kib=N": the arenas' totals, the largest tags by committed bytes, and the GPU atlases' committed bytes. A
// summary other than the startup one follows the previous by at least a minute.
void application_memory_summary(const char* reason) noexcept;
// The periodic summary, every ten minutes: a comparison per main-loop pass, no timer or wakeup of its own.
void application_memory_summary_tick(u64 now_ms) noexcept;
} // namespace maicord::app_detail
