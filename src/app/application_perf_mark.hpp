#pragma once
// Named startup points for MaiCordPerfProbe (tools/perf/perf_startup.hpp),
// reported per launch as microseconds since process creation with the main
// thread's CPU time. The production startup probe records the first time of
// each point. Shipping builds compile every mark away.
#if defined(MAICORD_PERF_PROBE)
#include "../../tools/perf/perf_startup.hpp"
#define MAICORD_PERF_MARK(name) ::maicord::perf::startup_mark(name)
#define MAICORD_PERF_FIRST_PRESENT() ::maicord::perf::startup_first_present()
#elif defined(MAICORD_STARTUP_PROBE)
#include "application_shutdown_metrics.hpp"
#define MAICORD_PERF_MARK(name) ::maicord::app_detail::application_startup_probe_mark(name)
#define MAICORD_PERF_FIRST_PRESENT() static_cast<void>(0)
#else
#define MAICORD_PERF_MARK(name) static_cast<void>(0)
#define MAICORD_PERF_FIRST_PRESENT() static_cast<void>(0)
#endif
