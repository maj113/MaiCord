#include "maicord/ui/controls/settings_controls.hpp"

namespace maicord {
namespace {
// Approximate advances (em) of a humanist UI sans for layout estimates.
float advance(u8 byte) noexcept {
    if ((byte & 0xc0) == 0x80)
        return 0;
    if (byte >= 0x80)
        return .56f;
    switch (byte) {
    case ' ':
        return .27f;
    case 'i':
    case 'l':
    case 'j':
    case '.':
    case ',':
    case ':':
    case ';':
    case '\'':
    case '!':
    case '|':
        return .25f;
    case 'I':
    case 'f':
    case '(':
    case ')':
    case '-':
        return .31f;
    case 't':
    case 'r':
        return .35f;
    case 'm':
    case 'M':
        return .86f;
    case 'w':
        return .73f;
    case 'W':
        return .94f;
    default:
        break;
    }
    if (byte >= 'A' && byte <= 'Z')
        return .62f;
    if (byte >= '0' && byte <= '9')
        return .55f;
    return .52f;
}
} // namespace

float ui_settings_text_width(const Utf8View& text, float em) noexcept {
    float width = 0;
    for (u32 i = 0; i < text.bytes; ++i)
        width += advance(static_cast<u8>(text.data[i]));
    return width * em;
}

u32 ui_settings_text_lines(const Utf8View& text, float em, float width) noexcept {
    if (!text.bytes)
        return 0;
    if (!(width > em))
        return 1;
    u32 lines = 1;
    float line = 0, word = 0;
    for (u32 i = 0; i <= text.bytes; ++i) {
        const u8 byte = i < text.bytes ? static_cast<u8>(text.data[i]) : ' ';
        if (byte == '\n' || byte == ' ') {
            if (line > 0 && line + word > width) {
                ++lines;
                line = word;
            } else
                line += word;
            word = 0;
            if (byte == '\n' && i < text.bytes) {
                ++lines;
                line = 0;
            } else
                line += advance(' ') * em;
            continue;
        }
        word += advance(byte) * em;
    }
    return lines;
}

} // namespace maicord
