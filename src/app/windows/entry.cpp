// The process entry of the Windows client: the probe builds' set-up, a second launch handing its discord:// link to the
// running client, then the application.
#include "../notifications/desktop.hpp"
#include "../shell/run.hpp"
#include "maicord/core/win32_kernel.hpp"

#if defined(MAICORD_PERF_PROBE)
#include "../../../tools/perf/perf_probe.hpp"
// Isolated probe builds never touch desktop integration, notifications, updates or another running instance.
#define MAICORD_ISOLATED_MAIN 1
#endif

extern "C" __declspec(noreturn) void WINAPI maicord_entry() noexcept {
#if defined(MAICORD_STARTUP_PROBE)
    if (!maicord::app_detail::application_startup_probe_initialize())
        ExitProcess(126);
#endif
#if defined(MAICORD_PERF_PROBE)
    maicord::perf::startup_trace_initialize();
    if (!maicord::perf::probe_initialize())
        ExitProcess(125);
#endif
#if !defined(MAICORD_ISOLATED_MAIN) && !defined(MAICORD_STARTUP_PROBE)
    const wchar_t* line = GetCommandLineW();
    maicord::u32 characters = 0;
    while (line[characters])
        ++characters;
    const maicord::ConstSlice<wchar_t> command{line, characters};
    // A second launch hands its discord:// link to the running client.
    if (maicord::app_detail::application_desktop_forward_existing(command))
        ExitProcess(0);
    maicord::app_detail::application_desktop_prepare(command);
#endif
    const int result = maicord::application_run();
    ExitProcess(static_cast<UINT>(result));
}
