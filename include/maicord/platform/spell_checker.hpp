#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/sys/spellcheck.hpp"

struct ISpellCheckerFactory;
struct ISpellChecker;

namespace maicord {

// Windows spell checking (ISpellCheckerFactory, Windows 8+) for the user's
// default locale. Opened lazily on first use from the UI thread; never from
// inside WM_CHAR handling. Words are UTF-8; longer words are not checked.
inline constexpr u32 kSpellCheckerWordBytes = 64;
inline constexpr u32 kSpellCheckerLanguageChars = 86; // LOCALE_NAME_MAX_LENGTH + 1

enum class SpellCheckerVerdict : u8 { unavailable, correct, misspelled };

struct SpellChecker {
    ISpellCheckerFactory* factory;
    ISpellChecker* checker;
    wchar_t language[kSpellCheckerLanguageChars];
    bool attempted, com_initialized;
};

// Chooses the checker language: the locale itself when supported, then its
// base language ("de" for "de-AT"), then "en-US". `supported` is a probe
// (IsSupported in production). Writes a NUL-terminated tag; false when none.
bool spell_checker_choose_language(const wchar_t* locale, bool (*supported)(void*, const wchar_t*) noexcept,
                                   void* context, Slice<wchar_t> output) noexcept;
// Idempotent; one attempt per process unless released. False when unavailable.
bool spell_checker_open(SpellChecker*) noexcept;
SpellCheckerVerdict spell_checker_check(SpellChecker*, Utf8View word) noexcept;
// Up to output.count suggestions, each a NUL-free UTF-8 view into storage.
u32 spell_checker_suggest(SpellChecker*, Utf8View word, Slice<char> storage, Slice<Utf8View> output) noexcept;
// Add writes the per-user Windows dictionary; Ignore lasts for this checker.
bool spell_checker_add(SpellChecker*, Utf8View word) noexcept;
bool spell_checker_ignore(SpellChecker*, Utf8View word) noexcept;
void spell_checker_release(SpellChecker*) noexcept;

} // namespace maicord
