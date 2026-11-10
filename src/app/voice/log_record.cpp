#include "maicord/app/voice/log.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
namespace {
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

void voice_log_fields(VoiceLogLevel level, const char* event, ConstSlice<VoiceLogField> fields) noexcept {
    if (!event)
        return;
    char line[256];
    u32 used = 0;
    constexpr u32 capacity = sizeof(line) - 8; // Cut where the "voice:<I|W|E> " record was (the prefix is 8 bytes).
    append(line, capacity, &used, event);
    for (u32 i = 0; i != fields.count; ++i) {
        append(line, capacity, &used, " ");
        append(line, capacity, &used, fields[i].key);
        append(line, capacity, &used, "=");
        append_number(line, capacity, &used, fields[i].value);
    }
    line[used] = 0;
    log_write(level == VoiceLogLevel::error     ? LogLevel::error
              : level == VoiceLogLevel::warning ? LogLevel::warning
                                                : LogLevel::info,
              "voice", line);
}
} // namespace maicord
