// The parts of log.hpp that are the same on both systems, included by src/core/log.cpp (Windows) and
// src/core/linux/log.cpp. Each of them defines begin_line and commit_line, between which a line is written.

namespace maicord {
namespace {

// Appends to a line, dropping what does not fit.
struct LineWriter {
    char* at;
    char* end;

    void put(const char* text) noexcept {
        for (; *text != 0 && at != end; ++text)
            *at++ = *text;
    }
};

// The low `digits` decimal digits of value, zero-padded.
void put_fixed_decimal(char* output, u32 value, u32 digits) noexcept {
    while (digits != 0) {
        output[--digits] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
}

char level_letter(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::trace:
        return 'T';
    case LogLevel::info:
        return 'I';
    case LogLevel::warning:
        return 'W';
    case LogLevel::error:
        return 'E';
    }
    return '?';
}

// "YYYY-MM-DD HH:MM:SS", the 19 characters of a line's time stamp before its milliseconds.
void put_stamp(char* stamp, u32 year, u32 month, u32 day, u32 hour, u32 minute, u32 second) noexcept {
    put_fixed_decimal(stamp, year, 4);
    stamp[4] = '-';
    put_fixed_decimal(stamp + 5, month, 2);
    stamp[7] = '-';
    put_fixed_decimal(stamp + 8, day, 2);
    stamp[10] = ' ';
    put_fixed_decimal(stamp + 11, hour, 2);
    stamp[13] = ':';
    put_fixed_decimal(stamp + 14, minute, 2);
    stamp[16] = ':';
    put_fixed_decimal(stamp + 17, second, 2);
}

// Takes the lock and starts a line after the buffered bytes, room for a whole line made first:
// "YYYY-MM-DD HH:MM:SS.mmm [L] [component] ".
LineWriter begin_line(LogLevel level, const char* component) noexcept;
// Ends the line begun by begin_line and releases the lock.
void commit_line(LineWriter writer, LogLevel level) noexcept;

} // namespace

void log_write(LogLevel level, const char* component, const char* message) noexcept {
    LineWriter line = begin_line(level, component);
    if (message) {
        line.put(message);
    }
    commit_line(line, level);
}

void log_u64(LogLevel level, const char* component, const char* key, u64 value) noexcept {
    char text[22];
    text[0] = '=';
    text[1 + decimal_write(text + 1, value)] = 0;
    LineWriter line = begin_line(level, component);
    line.put(key ? key : "value");
    line.put(text);
    commit_line(line, level);
}

} // namespace maicord
