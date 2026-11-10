#include "maicord/app/voice/log.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
#if defined(MAICORD_TEST_SEAMS)
void* volatile g_sink = nullptr;
#endif
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

#if defined(MAICORD_TEST_SEAMS)
void voice_log_set_sink(VoiceLogSink sink) noexcept {
    InterlockedExchangePointer(&g_sink, reinterpret_cast<void*>(sink));
}
#endif
void voice_log_fields(VoiceLogLevel level, const char* event, ConstSlice<VoiceLogField> fields) noexcept {
    char line[256];
    u32 used = 0;
#if defined(MAICORD_TEST_SEAMS)
    const auto sink = reinterpret_cast<VoiceLogSink>(g_sink); // an aligned pointer load; nothing else is ordered by it
    if (!sink || !event)
        return;
    constexpr u32 capacity = sizeof(line);
    append(line, capacity, &used,
           level == VoiceLogLevel::error ? "voice:E " : level == VoiceLogLevel::warning ? "voice:W " : "voice:I ");
#else
    if (!event)
        return;
    constexpr u32 capacity = sizeof(line) - 8; // Cut where the "voice:<I|W|E> " record was (the prefix is 8 bytes).
#endif
    append(line, capacity, &used, event);
    for (u32 i = 0; i != fields.count; ++i) {
        append(line, capacity, &used, " ");
        append(line, capacity, &used, fields[i].key);
        append(line, capacity, &used, "=");
        append_number(line, capacity, &used, fields[i].value);
    }
    line[used] = 0;
#if defined(MAICORD_TEST_SEAMS)
    sink(line);
#else
    log_write(level == VoiceLogLevel::error     ? LogLevel::error
              : level == VoiceLogLevel::warning ? LogLevel::warning
                                                : LogLevel::info,
              "voice", line);
#endif
}
void voice_log(VoiceLogLevel level, const char* event) noexcept {
    voice_log_fields(level, event, {});
}
void voice_log(VoiceLogLevel level, const char* event, const char* key, u64 value) noexcept {
    const VoiceLogField fields[]{{key, value}};
    voice_log_fields(level, event, {fields, 1});
}
void voice_log(VoiceLogLevel level, const char* event, const char* k1, u64 v1, const char* k2, u64 v2) noexcept {
    const VoiceLogField fields[]{{k1, v1}, {k2, v2}};
    voice_log_fields(level, event, {fields, 2});
}
void voice_log(VoiceLogLevel level, const char* event, const char* k1, u64 v1, const char* k2, u64 v2,
               const char* k3, u64 v3) noexcept {
    const VoiceLogField fields[]{{k1, v1}, {k2, v2}, {k3, v3}};
    voice_log_fields(level, event, {fields, 3});
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
