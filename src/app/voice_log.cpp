#include "maicord/app/voice_log.hpp"
#include "maicord/core/decimal.hpp"

#include <Windows.h>

namespace maicord {
namespace {
void* volatile g_sink = nullptr;
void append(char* line, u32 capacity, u32* used, const char* text) noexcept {
    while (text && *text && *used + 1 < capacity)
        line[(*used)++] = *text++;
}
void append_number(char* line, u32 capacity, u32* used, u64 value) noexcept {
    char digits[21];
    digits[decimal_write(digits, value)] = 0;
    append(line, capacity, used, digits);
}
} // namespace

void voice_log_set_sink(VoiceLogSink sink) noexcept {
    InterlockedExchangePointer(&g_sink, reinterpret_cast<void*>(sink));
}
void voice_log_fields(VoiceLogLevel level, const char* event, ConstSlice<VoiceLogField> fields) noexcept {
    const auto sink =
        reinterpret_cast<VoiceLogSink>(InterlockedCompareExchangePointer(&g_sink, nullptr, nullptr));
    if (!sink || !event)
        return;
    char line[256];
    u32 used = 0;
    append(line, sizeof(line), &used,
           level == VoiceLogLevel::error ? "voice:E " : level == VoiceLogLevel::warning ? "voice:W " : "voice:I ");
    append(line, sizeof(line), &used, event);
    for (u32 i = 0; i != fields.count; ++i) {
        append(line, sizeof(line), &used, " ");
        append(line, sizeof(line), &used, fields[i].key);
        append(line, sizeof(line), &used, "=");
        append_number(line, sizeof(line), &used, fields[i].value);
    }
    line[used] = 0;
    sink(line);
}
bool voice_log_limit(VoiceLogLimiter* limiter, u64 now_ms, u64* suppressed) noexcept {
    if (suppressed)
        *suppressed = 0;
    if (!limiter)
        return false;
    if (limiter->next_ms && now_ms < limiter->next_ms) {
        ++limiter->suppressed;
        return false;
    }
    if (suppressed)
        *suppressed = limiter->suppressed;
    limiter->suppressed = 0;
    limiter->next_ms = now_ms + kVoiceLogLimitIntervalMs;
    return true;
}
} // namespace maicord
