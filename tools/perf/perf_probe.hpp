#pragma once
// Hooks the client calls when it is built as MaiCordPerfProbe
// (MAICORD_PERF_PROBE). Shipping builds never include this header.
#include "perf_shared.hpp"

namespace maicord::perf {

// Process entry: validates the isolated profile, installs allocation
// counters and creates the synthetic account's profile marker. False exits.
bool probe_initialize() noexcept;
// After each main-loop wait.
void probe_observe_wait(u32 timeout_ms, bool timeout, bool frame_signal, bool message, bool handle) noexcept;
// End of each main-loop turn (after present). True ends the loop.
bool probe_step(int* exit_code) noexcept;
// After shutdown: writes the report to standard output.
void probe_finish(int exit_code) noexcept;

} // namespace maicord::perf
