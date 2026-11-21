#include "../account/account_ui_runtime.hpp"
#include "state.hpp"

#include "maicord/app/chat/composer/message_draft_session.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/path.hpp"

#if defined(MAICORD_STARTUP_PROBE)
#include "../notifications/desktop.hpp"
#include "maicord/core/trace_record.hpp"
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
    u32 entry_modules;
    StartupMark marks[kStartupMarkCapacity];
    u32 mark_count;
    // The record taken at the first Present, written after the loop turn's QR sign-in start.
    char line[sizeof(LogEvent::text)];
    bool initialized, recorded, written;
};
StartupProbe g_startup_probe{};

u64 startup_probe_us(i64 at) noexcept {
    return g_startup_probe.creation_offset_us +
           clock_microseconds(g_startup_probe.clock, g_startup_probe.correlation_qpc, at);
}

u64 filetime_value(FILETIME value) noexcept {
    return static_cast<u64>(value.dwHighDateTime) << 32 | value.dwLowDateTime;
}

// The DLLs that the process has mapped, the image included.
u32 loaded_module_count() noexcept {
    HMODULE first = nullptr;
    DWORD needed = 0;
    K32EnumProcessModules(current_process(), &first, sizeof(first), &needed);
    return needed / sizeof(HMODULE);
}

bool startup_probe_profile_marked() noexcept {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_STARTUP_BENCHMARK", enabled, 2) != 1 || enabled[0] != L'1')
        return false;
    wchar_t local[1024]{};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 1000);
    if (!count || count >= 1000 || path_root({local, count}, kPathRootSlash | kPathRootUnc) == PathRoot::none)
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
        !GetProcessTimes(current_process(), &created, &exited, &kernel, &user) ||
        filetime_value(precise) < filetime_value(created) || !startup_probe_profile_marked())
        return false;
    g_startup_probe.correlation_qpc = before + (after - before) / 2;
    g_startup_probe.creation_offset_us = (filetime_value(precise) - filetime_value(created)) / 10;
    g_startup_probe.entry_modules = loaded_module_count();
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
    const bool process_cycles_ok = QueryProcessCycleTime(current_process(), &process_cycles) != FALSE;
    const bool main_cycles_ok = QueryThreadCycleTime(current_thread(), &main_cycles) != FALSE;
    g_startup_probe.recorded = true;
    FILETIME created{}, exited{}, process_kernel{}, process_user{}, main_kernel{}, main_user{};
    const bool process_times_ok =
        GetProcessTimes(current_process(), &created, &exited, &process_kernel, &process_user) != FALSE;
    const bool main_times_ok =
        GetThreadTimes(current_thread(), &created, &exited, &main_kernel, &main_user) != FALSE;
    PROCESS_MEMORY_COUNTERS_EX2 memory{};
    memory.cb = sizeof(memory);
    bool memory_ok =
        GetProcessMemoryInfo(current_process(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                             sizeof(memory)) != FALSE;
    const bool pws_ok = memory_ok;
    if (!memory_ok) {
        // Older systems can still report commit/working set without EX2's
        // private working set. The separate status keeps that absence explicit.
        memory.cb = sizeof(PROCESS_MEMORY_COUNTERS_EX);
        memory_ok =
            GetProcessMemoryInfo(current_process(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                 sizeof(PROCESS_MEMORY_COUNTERS_EX)) != FALSE;
    }
    TraceRecord line("MAICORD_STARTUP");
    line.field("ttff_us", startup_probe_us(presented));
    line.field("app_ttff_us", clock_microseconds(g_app.clock, g_app.process_start, presented));
    line.field("entry_us", g_startup_probe.creation_offset_us);
    line.field("entry_modules", g_startup_probe.entry_modules);
    line.field("modules", loaded_module_count());
    line.field("page_faults", static_cast<u64>(memory.PageFaultCount));
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
    line.field("present_qpc", static_cast<u64>(presented));
    line.field("qpc_hz", static_cast<u64>(g_startup_probe.clock.frequency));
    copy_bytes(g_startup_probe.line, line.text, line.used + 1);
}

void application_startup_probe_finish() noexcept {
    if (!g_startup_probe.recorded || g_startup_probe.written)
        return;
    g_startup_probe.written = true;
    LogEvent line(g_startup_probe.line);
    line.field("backend_loaded", static_cast<u64>(g_app.account.module != nullptr));
    line.field("login_active", static_cast<u64>(g_app.account.login_instance != nullptr));
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
    window_request_close(g_app.window);
}
#endif

void application_persist_drafts_on_exit() noexcept {
    if (g_app.account_ui)
        message_draft_session_persist(g_app.account_ui->message_draft_session, g_app.account_ui->message_drafts,
                                      clock_monotonic_milliseconds(), true);
}

} // namespace maicord::app_detail
