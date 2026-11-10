#include "maicord/app/voice/log.hpp"

namespace maicord {
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
