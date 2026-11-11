// The Linux spell checker (src/platform/desktop/linux/spell_checker.cpp) with a dictionary of the repository
// (tests/fixtures/hunspell/xx_XX, found through $DICPATH) so the cases do not depend on the host's dictionaries, and
// with the installed ones for the choice of language.
// argv[1]:
//   fixture            LANG=xx_XX.UTF-8: check, suggestions, UTF-8, the personal word list, Ignore
//   language <tag>     the language the environment selects is <tag> ("de-DE", "de", "en-US"); needs the dictionaries
//                      of the Ubuntu hunspell packages (hunspell-en-us, hunspell-de-de) when MAICORD_TEST_SPELL_SET is
//                      "ubuntu", and is skipped where the wanted dictionary is missing otherwise
//   missing            the Hunspell and Enchant libraries are replaced by names that do not exist: unavailable
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/desktop/spell_checker.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"

#include "../../support/test_output.hpp"

namespace maicord {
namespace {

u32 g_checks = 0;
u32 g_failures = 0;

void check(bool condition, const char* name) noexcept {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        test_write_text("FAIL ");
        test_write_line(name);
    }
}

bool equal(const char* a, const char* b) noexcept {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

bool language_is(const SpellChecker& spell, const char* tag) noexcept {
    u32 i = 0;
    for (; tag[i]; ++i)
        if (spell.language[i] != static_cast<wchar_t>(tag[i]))
            return false;
    return spell.language[i] == 0;
}

SpellCheckerVerdict verdict(SpellChecker* spell, const char* word) noexcept {
    return spell_checker_check(spell, {word, c_string_length(word)});
}

bool suggests(SpellChecker* spell, const char* word, const char* wanted) noexcept {
    char storage[512];
    Utf8View suggestions[8]{};
    const u32 count = spell_checker_suggest(spell, {word, c_string_length(word)}, {storage, sizeof(storage)},
                                            {suggestions, 8});
    const u32 length = c_string_length(wanted);
    for (u32 i = 0; i < count; ++i)
        if (suggestions[i].bytes == length && bytes_equal(suggestions[i].data, wanted, length))
            return true;
    return false;
}

// The personal word file of the language, so that a rerun starts clean (the harness gives each test its own data
// directory, but not each run).
bool personal_file(const char* tag, char (&path)[2304]) noexcept {
    wchar_t relative[64]{};
    const char prefix[] = "spellcheck/";
    u32 length = 0;
    for (u32 i = 0; i < sizeof(prefix) - 1; ++i)
        relative[length++] = static_cast<wchar_t>(prefix[i]);
    for (u32 i = 0; tag[i]; ++i)
        relative[length++] = static_cast<wchar_t>(tag[i]);
    const char suffix[] = ".txt";
    for (u32 i = 0; i < sizeof(suffix); ++i)
        relative[length++] = static_cast<wchar_t>(suffix[i]);
    wchar_t absolute[768]{};
    return data_path(absolute, 768, relative, false).ok() && path_to_utf8(absolute, path, sizeof(path)) != 0;
}

void remove_directories(char (&path)[2304], u32 levels) noexcept {
    for (u32 level = 0; level < levels; ++level) {
        u32 end = c_string_length(path);
        while (end && path[end - 1] != '/')
            --end;
        path[end ? end - 1 : 0] = 0;
        sys_remove_directory(path);
    }
}

void fixture() noexcept {
    char path[2304]{};
    const bool has_path = personal_file("xx-XX", path);
    check(has_path, "personal_file_path");
    if (has_path)
        sys_unlink(path);

    SpellChecker spell{};
    check(spell_checker_open(&spell) && language_is(spell, "xx-XX"), "opens_the_fixture_dictionary");
    check(spell_checker_open(&spell), "open_is_idempotent");
    check(verdict(&spell, "maicord") == SpellCheckerVerdict::correct, "known_word");
    check(verdict(&spell, "Maicord") == SpellCheckerVerdict::correct, "capitalized_known_word");
    check(verdict(&spell, "MAICORD") == SpellCheckerVerdict::correct, "upper_case_known_word");
    check(verdict(&spell, "maicrod") == SpellCheckerVerdict::misspelled, "typo_is_misspelled");
    check(verdict(&spell, "caf\xC3\xA9") == SpellCheckerVerdict::correct, "accented_word_is_checked");
    check(verdict(&spell, "cafe") == SpellCheckerVerdict::misspelled, "missing_accent_is_misspelled");
    check(suggests(&spell, "maicrod", "maicord"), "typo_suggests_the_correction");
    check(suggests(&spell, "caf", "caf\xC3\xA9"), "suggestion_is_utf8");

    // What is not checked.
    check(spell_checker_check(&spell, {}) == SpellCheckerVerdict::unavailable, "empty_word");
    char long_word[80];
    for (char& c : long_word)
        c = 'a';
    check(spell_checker_check(&spell, {long_word, 65}) == SpellCheckerVerdict::unavailable, "word_over_64_bytes");
    check(spell_checker_check(&spell, {long_word, 64}) != SpellCheckerVerdict::unavailable, "word_of_64_bytes");
    check(verdict(&spell, "caf\xE9") == SpellCheckerVerdict::unavailable, "invalid_utf8");
    const char with_nul[] = {'a', 0, 'b'};
    check(spell_checker_check(&spell, {with_nul, 3}) == SpellCheckerVerdict::unavailable, "embedded_nul");

    // Suggestions are bounded by the output and by the storage.
    char storage[512];
    Utf8View views[1]{};
    check(spell_checker_suggest(&spell, {"maicrod", 7}, {storage, sizeof(storage)}, {views, 1}) <= 1,
          "suggestions_fit_the_output");
    check(spell_checker_suggest(&spell, {"maicrod", 7}, {storage, 3}, {views, 1}) == 0, "storage_too_small");
    check(spell_checker_suggest(&spell, {"maicrod", 7}, {storage, sizeof(storage)}, {nullptr, 0}) == 0,
          "no_output");

    // Ignore lasts for this checker only.
    check(verdict(&spell, "flarg") == SpellCheckerVerdict::misspelled, "unknown_before_ignore");
    check(spell_checker_ignore(&spell, {"flarg", 5}) && verdict(&spell, "flarg") == SpellCheckerVerdict::correct,
          "ignored_word_is_correct");
    // Add keeps the word in the personal list.
    check(verdict(&spell, "zorp") == SpellCheckerVerdict::misspelled, "unknown_before_add");
    check(spell_checker_add(&spell, {"zorp", 4}) && verdict(&spell, "zorp") == SpellCheckerVerdict::correct,
          "added_word_is_correct");
    check(spell_checker_add(&spell, {"zorp", 4}), "adding_twice_succeeds");
    check(!spell_checker_add(&spell, {}) && !spell_checker_ignore(&spell, {}), "empty_word_is_not_added");
    spell_checker_release(&spell);
    check(!spell.checker && !spell.factory, "release_clears_the_checker");

    SpellChecker again{};
    check(spell_checker_open(&again), "reopens");
    check(verdict(&again, "zorp") == SpellCheckerVerdict::correct, "added_word_survives_a_new_checker");
    check(verdict(&again, "flarg") == SpellCheckerVerdict::misspelled, "ignored_word_does_not");
    spell_checker_release(&again);
    spell_checker_release(&again);
    if (has_path) {
        sys_unlink(path);
        remove_directories(path, 2);
    }
}

bool skipped_for_missing_dictionary() noexcept {
    const char* set = environment_get("MAICORD_TEST_SPELL_SET");
    return !(set && equal(set, "ubuntu"));
}

void language(const char* wanted) noexcept {
    SpellChecker spell{};
    const bool opened = spell_checker_open(&spell);
    if (!opened || !language_is(spell, wanted)) {
        if (skipped_for_missing_dictionary()) {
            test_write_text("SKIP the dictionary of ");
            test_write_text(wanted);
            test_write_line(" is not installed");
            spell_checker_release(&spell);
            return;
        }
        check(opened && language_is(spell, wanted), "environment_selects_the_language");
        spell_checker_release(&spell);
        return;
    }
    if (language_is(spell, "en-US")) {
        check(verdict(&spell, "hello") == SpellCheckerVerdict::correct, "english_word");
        check(verdict(&spell, "helo") == SpellCheckerVerdict::misspelled, "english_typo");
    } else if (language_is(spell, "de-DE") || language_is(spell, "de")) {
        check(verdict(&spell, "Haus") == SpellCheckerVerdict::correct, "german_word");
        check(verdict(&spell, "Hauss") == SpellCheckerVerdict::misspelled, "german_typo");
        check(verdict(&spell, "Stra\xC3\x9F" "e") == SpellCheckerVerdict::correct, "german_sharp_s");
    }
    spell_checker_release(&spell);
}

void missing() noexcept {
    SpellChecker spell{};
    check(!spell_checker_open(&spell), "no_library_means_unavailable");
    check(!spell_checker_open(&spell), "one_attempt_only");
    check(verdict(&spell, "hello") == SpellCheckerVerdict::unavailable, "check_is_unavailable");
    char storage[64];
    Utf8View views[2]{};
    check(spell_checker_suggest(&spell, {"helo", 4}, {storage, sizeof(storage)}, {views, 2}) == 0, "no_suggestions");
    check(!spell_checker_add(&spell, {"zorp", 4}) && !spell_checker_ignore(&spell, {"zorp", 4}), "no_add_or_ignore");
    spell_checker_release(&spell);
    check(!spell.checker && !spell.factory && !spell.attempted, "release_resets_the_attempt");
    check(spell_checker_open(nullptr) == false && spell_checker_check(nullptr, {"a", 1}) ==
                                                      SpellCheckerVerdict::unavailable,
          "null_checker");
}

bool run(const char* mode, const char* argument) noexcept {
    if (equal(mode, "fixture"))
        fixture();
    else if (equal(mode, "language"))
        language(argument);
    else if (equal(mode, "missing"))
        missing();
    else {
        test_write_line("unknown mode");
        return false;
    }
    return g_failures == 0;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_spellcheck_linux_entry) {
    const maicord::ProcessStartup& startup = maicord::process_startup();
    const bool ok = maicord::run(startup.argc > 1 ? startup.argv[1] : "", startup.argc > 2 ? startup.argv[2] : "");
    maicord::test_write_metric("checks=", maicord::g_checks);
    maicord::test_exit(ok ? 0 : 1, "maicord_spellcheck_linux");
}
