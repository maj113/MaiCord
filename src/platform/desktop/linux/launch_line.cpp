#include "maicord/platform/desktop/launch_line.hpp"
#include "maicord/os/text.hpp"
#include "maicord/platform/desktop/integration.hpp"

namespace maicord {
namespace {

struct LineWriter {
    wchar_t* data;
    u32 capacity;
    u32 count;
    bool full;

    void put(wchar_t unit) noexcept {
        if (count < capacity)
            data[count++] = unit;
        else
            full = true;
    }
};

bool needs_quotes(const wchar_t* text, u32 units) noexcept {
    if (units == 0)
        return true;
    for (u32 i = 0; i < units; ++i)
        if (text[i] == L' ' || text[i] == L'\t' || text[i] == L'"')
            return true;
    return false;
}

void put_argument(LineWriter* line, const wchar_t* text, u32 units) noexcept {
    const bool quote = needs_quotes(text, units);
    if (quote)
        line->put(L'"');
    u32 slashes = 0;
    for (u32 i = 0; i < units; ++i) {
        const wchar_t unit = text[i];
        if (unit == L'\\') {
            ++slashes;
            continue;
        }
        // 2n+1 backslashes and a quote are n literal backslashes and a literal quote: double them, then escape it.
        for (u32 s = 0; s < (unit == L'"' ? slashes * 2 + 1 : slashes); ++s)
            line->put(L'\\');
        slashes = 0;
        line->put(unit);
    }
    // Backslashes before the closing quote would escape it.
    for (u32 s = 0; s < (quote ? slashes * 2 : slashes); ++s)
        line->put(L'\\');
    if (quote)
        line->put(L'"');
}

} // namespace

u32 desktop_launch_line(int argc, const char* const* argv, wchar_t* output, u32 capacity) noexcept {
    LineWriter line{output, capacity, 0, false};
    for (int index = 0; index < argc; ++index) {
        const char* argument = argv[index];
        u32 bytes = 0;
        while (argument[bytes])
            ++bytes;
        wchar_t wide[kDesktopUrlCapacity + 8];
        const u32 units = utf8_to_utf16(argument, bytes, wide, sizeof(wide) / sizeof(wide[0]), false);
        // An argument that does not fit its buffer is no argument: the line ends before it.
        if (bytes != 0 && units == 0)
            break;
        LineWriter attempt = line;
        if (index != 0)
            attempt.put(L' ');
        if (index == 0) {
            attempt.put(L'"');
            for (u32 i = 0; i < units; ++i)
                if (wide[i] != L'"')
                    attempt.put(wide[i]);
            attempt.put(L'"');
        } else {
            put_argument(&attempt, wide, units);
        }
        if (attempt.full)
            break;
        line = attempt;
    }
    return line.count;
}

} // namespace maicord
