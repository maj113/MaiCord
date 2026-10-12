#pragma once
// Startup attribution for MaiCordPerfProbe (tools/perf): a main-thread
// timeline of named points, every DLL the process maps, timed system calls
// (window, D3D11, DXGI, DirectComposition, COM, module loads) and the D3D11
// objects created before the first presented frame. Shipping builds never
// include this header; MAICORD_PERF_MARK compiles to nothing there.

namespace maicord::perf {

// First call at process entry, before anything else runs.
void startup_trace_initialize() noexcept;
// A named main-thread point; only the first occurrence of each name before
// the first presented frame is kept.
void startup_mark(const char* name) noexcept;
// The first presented frame ends the attribution window.
void startup_first_present() noexcept;
// Writes the startup lines to standard output (ahead of the probe report).
void startup_trace_report() noexcept;

} // namespace maicord::perf
