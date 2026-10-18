// Real Windows spell checker smoke test (CTest label system_spellcheck).
// Skips when the factory or a language is unavailable. Read-only: it never
// adds or ignores words, so the user's dictionary is untouched.
#include "maicord/core/memory.hpp"
#include "maicord/platform/spell_checker.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
u32 g_failures;
void print(const char* text) noexcept {
    u32 bytes = 0;
    while (text[bytes])
        ++bytes;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, bytes, &written, nullptr);
}
void expect(bool condition, const char* name) noexcept {
    if (condition)
        return;
    ++g_failures;
    print("FAIL ");
    print(name);
    print("\r\n");
}
u32 run() noexcept {
    SpellChecker spell{};
    if (!spell_checker_open(&spell)) {
        spell_checker_release(&spell);
        print("SKIP system spell checker unavailable\r\n");
        return 0;
    }
    const bool english = spell.language[0] == L'e' && spell.language[1] == L'n';
    expect(spell_checker_check(&spell, utf8_literal("qzxvbnmqwk")) == SpellCheckerVerdict::misspelled,
           "nonsense word is misspelled");
    expect(spell_checker_check(&spell, {}) == SpellCheckerVerdict::unavailable, "empty word is not checked");
    char storage[320];
    Utf8View suggestions[5]{};
    const u32 count = spell_checker_suggest(&spell, utf8_literal("qzxvbnmqwk"), {storage, sizeof(storage)},
                                            {suggestions, 5});
    expect(count <= 5, "suggestions are bounded");
    if (english) {
        expect(spell_checker_check(&spell, utf8_literal("hello")) == SpellCheckerVerdict::correct,
               "common English word is correct");
        expect(spell_checker_check(&spell, utf8_literal("helo")) == SpellCheckerVerdict::misspelled,
               "English typo is misspelled");
        const u32 typo = spell_checker_suggest(&spell, utf8_literal("helo"), {storage, sizeof(storage)},
                                               {suggestions, 5});
        bool hello = false;
        for (u32 i = 0; i < typo; ++i)
            hello = hello || (suggestions[i].bytes == 5 && bytes_equal(suggestions[i].data, "hello", 5));
        expect(typo > 0 && typo <= 5 && hello, "English typo suggests the correction");
    }
    spell_checker_release(&spell);
    expect(!spell.checker && !spell.factory, "release clears the checker");
    if (!g_failures)
        print("PASS system_spellcheck\r\n");
    return g_failures;
}
} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_system_spellcheck_entry() noexcept {
    ExitProcess(maicord::run());
}
