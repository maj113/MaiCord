// The log of the UI probe (MaiCordUiProbe), linked in place of src/core/log.cpp: each line goes to standard output,
// where CTest keeps it beside the probe's result, in the client's line format; nothing reaches maicord.log.
#include "maicord/core/log.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_system_time.hpp"

#include "../../src/core/log_shared.inc"

namespace maicord {
namespace {

constexpr u32 kLineBytes = 1023;
constexpr u32 kStampCharacters = 19; // "YYYY-MM-DD HH:MM:SS"
constexpr u32 kPrefixCharacters = kStampCharacters + 10; // stamp, ".mmm [L] ["

SRWLOCK g_lock{};
LogCounters g_counters{};
char g_line[kLineBytes];

LineWriter begin_line(LogLevel level, const char* component) noexcept {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    AcquireSRWLockExclusive(&g_lock);
    put_stamp(g_line, time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    g_line[kStampCharacters] = '.';
    put_fixed_decimal(g_line + kStampCharacters + 1, time.wMilliseconds, 3);
    copy_bytes(g_line + kStampCharacters + 4, " [?] [", 6);
    g_line[kStampCharacters + 6] = level_letter(level);
    LineWriter writer{g_line + kPrefixCharacters, g_line + kLineBytes};
    writer.put(component ? component : "unknown");
    writer.put("] ");
    return writer;
}

void commit_line(LineWriter writer, LogLevel) noexcept {
    writer.put("\r\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), g_line, static_cast<DWORD>(writer.at - g_line), &written, nullptr);
    ++g_counters.lines;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace

bool log_open() noexcept {
    return true;
}

void log_close() noexcept {}

LogCounters log_counters() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const LogCounters counters = g_counters;
    ReleaseSRWLockExclusive(&g_lock);
    return counters;
}

} // namespace maicord
