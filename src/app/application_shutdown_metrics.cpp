#include "account_ui_runtime.hpp"
#include "application_state.hpp"

#include "maicord/app/message_draft_session.hpp"
#include "maicord/core/time.hpp"

#if defined(MAICORD_STARTUP_PROBE)
#include "application_desktop.hpp"
#include "maicord/core/trace_record.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/process_info.hpp"
#endif

namespace maicord::app_detail {
namespace {

#if defined(MAICORD_STARTUP_PROBE)
struct StartupMark {
    const char* name;
    i64 at;
};
constexpr u32 kStartupMarkCapacity = 48;
struct StartupProbe {
    Clock clock;
    i64 correlation_qpc;
    u64 creation_offset_us;
    StartupMark marks[kStartupMarkCapacity];
    u32 mark_count;
    bool initialized, recorded;
};
StartupProbe g_startup_probe{};

u64 startup_probe_us(i64 at) noexcept {
    return g_startup_probe.creation_offset_us +
           clock_microseconds(g_startup_probe.clock, g_startup_probe.correlation_qpc, at);
}

u64 filetime_value(FILETIME value) noexcept {
    return static_cast<u64>(value.dwHighDateTime) << 32 | value.dwLowDateTime;
}

bool startup_probe_profile_marked() noexcept {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_STARTUP_BENCHMARK", enabled, 2) != 1 || enabled[0] != L'1')
        return false;
    wchar_t local[1024]{};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 1000);
    if (!count || count >= 1000 ||
        !((count >= 3 && local[1] == L':' && (local[2] == L'\\' || local[2] == L'/')) ||
          (count >= 2 && local[0] == L'\\' && local[1] == L'\\')))
        return false;
    static constexpr wchar_t marker[] = L"\\.maicord-test-profile";
    for (u32 i = 0; i < sizeof(marker) / sizeof(marker[0]); ++i)
        local[count + i] = marker[i];
    const DWORD attributes = GetFileAttributesW(local);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
#endif
} // namespace

#if defined(MAICORD_STARTUP_PROBE)
bool application_startup_probe_initialize() noexcept {
    // Correlate the QPC clock with process creation before marker checks or
    // any application work. Report the successful Present return, not scanout.
    g_startup_probe.clock = clock_initialize();
    const i64 before = clock_now();
    FILETIME precise{}, created{}, exited{}, kernel{}, user{};
    GetSystemTimePreciseAsFileTime(&precise);
    const i64 after = clock_now();
    if (g_startup_probe.clock.frequency <= 0 ||
        !GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) ||
        filetime_value(precise) < filetime_value(created) || !startup_probe_profile_marked())
        return false;
    g_startup_probe.correlation_qpc = before + (after - before) / 2;
    g_startup_probe.creation_offset_us = (filetime_value(precise) - filetime_value(created)) / 10;
    g_startup_probe.initialized = true;
    return true;
}

void application_startup_probe_mark(const char* name) noexcept {
    if (!g_startup_probe.initialized || g_startup_probe.recorded)
        return;
    for (u32 i = 0; i < g_startup_probe.mark_count; ++i)
        if (g_startup_probe.marks[i].name == name)
            return;
    if (g_startup_probe.mark_count < kStartupMarkCapacity)
        g_startup_probe.marks[g_startup_probe.mark_count++] = {name, clock_now()};
}

void application_startup_probe_first_present() noexcept {
    if (!g_startup_probe.initialized || g_startup_probe.recorded)
        return;
    const i64 presented = clock_now();
    ULONG64 process_cycles = 0, main_cycles = 0;
    const bool process_cycles_ok = QueryProcessCycleTime(GetCurrentProcess(), &process_cycles) != FALSE;
    const bool main_cycles_ok = QueryThreadCycleTime(GetCurrentThread(), &main_cycles) != FALSE;
    g_startup_probe.recorded = true;
    FILETIME created{}, exited{}, process_kernel{}, process_user{}, main_kernel{}, main_user{};
    const bool process_times_ok =
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &process_kernel, &process_user) != FALSE;
    const bool main_times_ok =
        GetThreadTimes(GetCurrentThread(), &created, &exited, &main_kernel, &main_user) != FALSE;
    PROCESS_MEMORY_COUNTERS_EX2 memory{};
    memory.cb = sizeof(memory);
    bool memory_ok =
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                             sizeof(memory)) != FALSE;
    const bool pws_ok = memory_ok;
    if (!memory_ok) {
        // Older systems can still report commit/working set without EX2's
        // private working set. The separate status keeps that absence explicit.
        memory.cb = sizeof(PROCESS_MEMORY_COUNTERS_EX);
        memory_ok =
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                 sizeof(PROCESS_MEMORY_COUNTERS_EX)) != FALSE;
    }
    TraceRecord line("MAICORD_STARTUP");
    line.field("ttff_us", startup_probe_us(presented));
    line.field("app_ttff_us", clock_microseconds(g_app.clock, g_app.process_start, presented));
    line.field("main_cycles", main_cycles);
    line.field("process_cycles", process_cycles);
    line.field("main_cpu_us", (filetime_value(main_kernel) + filetime_value(main_user)) / 10);
    line.field("process_cpu_us", (filetime_value(process_kernel) + filetime_value(process_user)) / 10);
    line.field("ws_bytes", static_cast<u64>(memory.WorkingSetSize));
    line.field("private_bytes", static_cast<u64>(memory.PrivateUsage));
    line.field("pws_bytes", pws_ok ? static_cast<u64>(memory.PrivateWorkingSetSize) : 0);
    line.field("cycles_ok", static_cast<u64>(process_cycles_ok && main_cycles_ok));
    line.field("times_ok", static_cast<u64>(process_times_ok && main_times_ok));
    line.field("memory_ok", static_cast<u64>(memory_ok));
    line.field("pws_ok", static_cast<u64>(pws_ok));
    line.field("backend_loaded", static_cast<u64>(g_app.account.module != nullptr));
    line.field("login_active", static_cast<u64>(g_app.account.login_instance != nullptr));
    line.field("present_qpc", static_cast<u64>(presented));
    line.field("qpc_hz", static_cast<u64>(g_startup_probe.clock.frequency));
    line.append("\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.text, line.used, &written, nullptr);
    // Marks are microseconds since process creation, like ttff_us.
    for (u32 i = 0; i < g_startup_probe.mark_count; ++i) {
        TraceRecord mark("MAICORD_STARTUP_MARK");
        mark.field("name", g_startup_probe.marks[i].name);
        mark.field("us", startup_probe_us(g_startup_probe.marks[i].at));
        mark.append("\n");
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), mark.text, mark.used, &written, nullptr);
    }
    application_desktop_request_quit();
    PostMessageW(g_app.window, WM_CLOSE, 0, 0);
}
#endif

void application_persist_drafts_on_exit() noexcept {
    if (g_app.account_ui)
        message_draft_session_persist(g_app.account_ui->message_draft_session, g_app.account_ui->message_drafts,
                                      clock_monotonic_milliseconds(), true);
}

} // namespace maicord::app_detail
