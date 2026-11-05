#include "maicord/core/memory.hpp"
#include "maicord/os/text.hpp"
#include "../support/test_output.hpp"

// UTF-16 and UTF-8 conversion (maicord/os/text.hpp): ASCII, two-, three- and four-byte sequences, surrogate pairs,
// strict and lenient handling of broken input, and the capacity rule (nothing partial is written). Windows runs the
// same cases through WideCharToMultiByte and MultiByteToWideChar.

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

bool same_bytes(const char* a, const char* b, u32 bytes) noexcept {
    return bytes_equal(a, b, bytes);
}

void test_to_utf8() noexcept {
    const wchar_t text[] = {L'a', 0x00E9, 0x20AC, 0xD83D, 0xDE00, L'z'}; // a, e acute, euro sign, an emoji, z
    char out[32];
    const char expected[] = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z";
    const u32 bytes = utf16_to_utf8(text, 6, out, sizeof(out), true);
    check(bytes == sizeof(expected) - 1 && same_bytes(out, expected, bytes), "UTF-16 to UTF-8, every sequence length");
    check(utf16_to_utf8(text, 0, out, sizeof(out), true) == 0, "empty text converts to nothing");
    check(utf16_to_utf8(text, 6, out, 8, true) == 0, "output too small writes nothing and reports zero");
    const wchar_t lone[] = {L'a', 0xD83D, L'b'};
    check(utf16_to_utf8(lone, 3, out, sizeof(out), true) == 0, "strict: an unpaired surrogate fails");
    const u32 lenient = utf16_to_utf8(lone, 3, out, sizeof(out), false);
    check(lenient == 5 && same_bytes(out, "a\xEF\xBF\xBD" "b", 5), "lenient: an unpaired surrogate becomes U+FFFD");
}

void test_to_utf16() noexcept {
    const char text[] = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z";
    wchar_t out[16];
    const u32 units = utf8_to_utf16(text, sizeof(text) - 1, out, 16, true);
    check(units == 6 && out[0] == L'a' && out[1] == 0x00E9 && out[2] == 0x20AC && out[3] == 0xD83D &&
              out[4] == 0xDE00 && out[5] == L'z',
          "UTF-8 to UTF-16, every sequence length");
    check(utf8_to_utf16(text, 0, out, 16, true) == 0, "empty text converts to nothing");
    check(utf8_to_utf16(text, sizeof(text) - 1, out, 4, true) == 0, "output too small reports zero");
    const char broken[] = "a\xFF" "b";
    check(utf8_to_utf16(broken, 3, out, 16, true) == 0, "strict: an invalid byte fails");
    check(utf8_to_utf16(broken, 3, out, 16, false) == 3 && out[0] == L'a' && out[1] == 0xFFFD && out[2] == L'b',
          "lenient: an invalid byte becomes U+FFFD");
    const char overlong[] = "\xC0\x80";
    check(utf8_to_utf16(overlong, 2, out, 16, true) == 0, "strict: an overlong form fails");
    const char truncated[] = "a\xE2\x82";
    check(utf8_to_utf16(truncated, 3, out, 16, true) == 0, "strict: a truncated sequence fails");
}

void test_round_trip() noexcept {
    wchar_t wide[64];
    char utf8[256];
    wchar_t original[64];
    u32 count = 0;
    for (u32 cp = 0x20; cp < 0x3000 && count < 60; cp += 0x131)
        original[count++] = static_cast<wchar_t>(cp);
    const u32 bytes = utf16_to_utf8(original, count, utf8, sizeof(utf8), true);
    const u32 units = bytes ? utf8_to_utf16(utf8, bytes, wide, 64, true) : 0;
    check(units == count && bytes_equal(wide, original, count * sizeof(wchar_t)), "a spread of code points round-trips");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_os_text_entry) {
    using namespace maicord;
    test_to_utf8();
    test_to_utf16();
    test_round_trip();
    test_exit(g_failures, "os_text");
}
