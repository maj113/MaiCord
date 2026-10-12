#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// Account-worker voice diagnostics (log category "voice"). Records hold a fixed
// event name and numeric fields only: never keys, MLS secrets, tokens, session
// ids, addresses or ports. The host installs one sink for the module; records
// are formatted "voice:<I|W|E> <event> key=value ..." for it to route.
using VoiceLogSink = void (*)(const char* record) noexcept;
enum class VoiceLogLevel : u8 {
    info,
    warning,
    error
};
struct VoiceLogField {
    const char* key;
    u64 value;
};
void voice_log_set_sink(VoiceLogSink sink) noexcept;
void voice_log_fields(VoiceLogLevel level, const char* event, ConstSlice<VoiceLogField> fields) noexcept;
inline void voice_log(VoiceLogLevel level, const char* event) noexcept {
    voice_log_fields(level, event, {});
}
inline void voice_log(VoiceLogLevel level, const char* event, const char* key, u64 value) noexcept {
    const VoiceLogField fields[]{{key, value}};
    voice_log_fields(level, event, {fields, 1});
}
inline void voice_log(VoiceLogLevel level, const char* event, const char* k1, u64 v1, const char* k2,
                      u64 v2) noexcept {
    const VoiceLogField fields[]{{k1, v1}, {k2, v2}};
    voice_log_fields(level, event, {fields, 2});
}
inline void voice_log(VoiceLogLevel level, const char* event, const char* k1, u64 v1, const char* k2, u64 v2,
                      const char* k3, u64 v3) noexcept {
    const VoiceLogField fields[]{{k1, v1}, {k2, v2}, {k3, v3}};
    voice_log_fields(level, event, {fields, 3});
}
// Per-packet failures: the first record, then at most one per interval that
// carries how many were suppressed in between. Zero-initialize.
struct VoiceLogLimiter {
    u64 next_ms, suppressed;
};
inline constexpr u64 kVoiceLogLimitIntervalMs = 5'000;
// True when a record may be written now; *suppressed receives the skipped count.
bool voice_log_limit(VoiceLogLimiter* limiter, u64 now_ms, u64* suppressed) noexcept;
} // namespace maicord
