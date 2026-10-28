#include "maicord/core/memory.hpp"
#include "maicord/platform/spell_checker.hpp"
#include "maicord/ui/composer_spellcheck.hpp"
#include "maicord/ui/popup_scene.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

// Tokens joined with '|' for compact expectations.
bool tokens(Utf8View text, Utf8View expected) noexcept {
    char joined[256];
    u32 bytes = 0;
    auto tokenizer = spell_tokenizer(text);
    SpellToken token{};
    while (spell_tokenizer_next(&tokenizer, &token)) {
        const u32 length = token.last - token.first;
        if (bytes + length + 1 > sizeof(joined))
            return false;
        if (bytes)
            joined[bytes++] = '|';
        copy_bytes(joined + bytes, text.data + token.first, length);
        bytes += length;
    }
    return bytes == expected.bytes && bytes_equal(joined, expected.data, bytes);
}

struct FakeChecker {
    char seen[16][32];
    u32 calls;
    u32 learned_adds, learned_ignores;
    bool unavailable;
};
// Misspelled: any word starting with 'x' plus "helo".
ComposerSpellVerdict fake_check(void* context, Utf8View word) noexcept {
    auto& fake = *static_cast<FakeChecker*>(context);
    if (fake.unavailable)
        return ComposerSpellVerdict::unavailable;
    if (fake.calls < 16) {
        const u32 bytes = minimum(word.bytes, 31u);
        copy_bytes(fake.seen[fake.calls], word.data, bytes);
        fake.seen[fake.calls][bytes] = 0;
    }
    ++fake.calls;
    return word.data[0] == 'x' || utf8_equal(word, utf8_literal("helo")) ? ComposerSpellVerdict::misspelled
                                                                         : ComposerSpellVerdict::correct;
}
u32 fake_suggest(void*, Utf8View, Slice<char> storage, Slice<Utf8View> output) noexcept {
    const char* values[] = {"hello", "help", "hell", "held", "helot", "heal", "hero"};
    u32 used = 0, count = 0;
    for (const char* value : values) {
        const u32 length = c_string_length(value);
        if (count == output.count || used + length > storage.count)
            break;
        copy_bytes(storage.data + used, value, length);
        output[count++] = {storage.data + used, length};
        used += length;
    }
    return count;
}
bool fake_learn(void* context, Utf8View, bool add) noexcept {
    auto& fake = *static_cast<FakeChecker*>(context);
    ++(add ? fake.learned_adds : fake.learned_ignores);
    return true;
}

void test_tokenizer(Check check) noexcept {
    check(tokens(utf8_literal("Hello there, wrld!"), utf8_literal("Hello|there|wrld")),
          "spell tokenizer yields plain words without punctuation");
    check(tokens(utf8_literal("don't it's 'quoted' dogs'"), utf8_literal("don't|it's|quoted|dogs")),
          "spell tokenizer keeps inner apostrophes and drops edge quotes");
    check(tokens(utf8_literal("it\xE2\x80\x99s fine"), utf8_literal("it\xE2\x80\x99s|fine")),
          "spell tokenizer keeps typographic apostrophes inside words");
    check(tokens(utf8_literal("na\xC3\xAFve \xC3\xBC" "ber stra\xC3\x9F" "e \xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"),
                 utf8_literal("na\xC3\xAFve|\xC3\xBC" "ber|stra\xC3\x9F" "e|\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82")),
          "spell tokenizer accepts Unicode letters");
    check(tokens(utf8_literal("cafe\xCC\x81 ok"), utf8_literal("cafe\xCC\x81|ok")),
          "spell tokenizer keeps combining marks with their word");
    check(tokens(utf8_literal("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E text"), utf8_literal("text")),
          "spell tokenizer leaves unspaced scripts alone");
    check(tokens(utf8_literal("a I x ok"), utf8_literal("ok")), "spell tokenizer skips single letters");
    check(tokens(utf8_literal("2nd 100 abc123 foo_bar v2 _em_"), utf8_literal("em")),
          "spell tokenizer skips numbers, digit words and identifiers");
    check(tokens(utf8_literal("see `inline code` and ``two `ticks` here`` done"), utf8_literal("see|and|done")),
          "spell tokenizer skips inline code spans");
    check(tokens(utf8_literal("before ```cpp\nint mian;\n``` after"), utf8_literal("before|after")),
          "spell tokenizer skips fenced code blocks");
    check(tokens(utf8_literal("open ```still typng"), utf8_literal("open")),
          "spell tokenizer skips an unclosed fence being typed");
    check(tokens(utf8_literal("lone ` tick"), utf8_literal("lone|tick")), "spell tokenizer treats unmatched ticks as text");
    check(tokens(utf8_literal("hi <@123> <@!45> <@&67> <#89> yo"), utf8_literal("hi|yo")),
          "spell tokenizer skips user, role and channel mentions");
    check(tokens(utf8_literal("<:blobcat:123> <a:wavey:456> <t:1700000000:R> </ban user:42> ok"), utf8_literal("ok")),
          "spell tokenizer skips custom emoji, timestamps and command mentions");
    check(tokens(utf8_literal("ping @someone and @everyone @here in #general-chat now"), utf8_literal("ping|and|in|now")),
          "spell tokenizer skips plain @name and #name tokens");
    check(tokens(utf8_literal("nice :thumbsup: :flag_us: time: now"), utf8_literal("nice|time|now")),
          "spell tokenizer skips emoji shortcodes but not a lone colon");
    check(tokens(utf8_literal("/gif cats"), utf8_literal("cats")), "spell tokenizer skips a leading slash command");
    check(tokens(utf8_literal("and/or /notcmd"), utf8_literal("and|or|notcmd")),
          "spell tokenizer checks slashes away from the message start");
    check(tokens(utf8_literal("go https://exampel.com/pth?q=wrd now"), utf8_literal("go|now")), "spell tokenizer skips URLs");
    check(tokens(utf8_literal("visit www.exampel.org or discrod.gg today"), utf8_literal("visit|or|today")),
          "spell tokenizer skips www and domain-like tokens");
    check(tokens(utf8_literal("<https://x.y/z> mail jhon.doe@exmaple.com bye"), utf8_literal("mail|bye")),
          "spell tokenizer skips autolinks and e-mail addresses");
    check(tokens(utf8_literal("# Heading **bold** ||spoilr||"), utf8_literal("Heading|bold|spoilr")),
          "spell tokenizer checks markdown-decorated words");
    check(tokens({}, {}) && tokens(utf8_literal("   "), {}), "spell tokenizer handles empty text");
}

ComposerSpellcheckDocument document(const char* text, u32 bytes, u32 revision, u32 caret, u64 identity = 7) noexcept {
    static const int model = 0;
    return {&model, identity, revision, caret, {text, bytes}};
}

void test_cache(Check check) noexcept {
    static ComposerSpellcheck spell{};
    spell = {};
    FakeChecker fake{};
    spell.backend = {&fake, fake_check, fake_suggest, fake_learn};

    // A loaded draft is checked at once; caret-word deferral starts with typing.
    char text[64] = "hello xyzzy world";
    check(composer_spellcheck_tick(&spell, document(text, 17, 1, 17), true, 1000) && spell.span_count == 1 &&
              spell.spans[0].start == 6 && spell.spans[0].end == 11 &&
              spell.spans[0].line_style == TextCompositionLineStyle::squiggle &&
              spell.spans[0].attribute == TextCompositionAttribute::input_error && fake.calls == 3,
          "spellcheck checks a loaded draft and marks the misspelled word");
    check(!composer_spellcheck_tick(&spell, document(text, 17, 1, 17), true, 1001) && fake.calls == 3 &&
              !composer_spellcheck_next_wake(spell),
          "spellcheck idles without edits");

    // Typing " xq": known words keep their marks immediately, new words wait.
    copy_bytes(text + 17, " xqa", 4);
    const u32 calls = fake.calls;
    composer_spellcheck_tick(&spell, document(text, 21, 2, 21), true, 2000);
    check(fake.calls == calls && spell.span_count == 1 && composer_spellcheck_next_wake(spell) == 2000 + kComposerSpellcheckDebounceMs,
          "spellcheck debounces edits without querying");
    composer_spellcheck_tick(&spell, document(text, 21, 2, 21), true, 2000 + kComposerSpellcheckDebounceMs);
    check(fake.calls == calls && spell.span_count == 1, "spellcheck skips the word still being typed");
    composer_spellcheck_tick(&spell, document(text, 22, 3, 22), true, 2400);
    text[21] = ' ';
    composer_spellcheck_tick(&spell, document(text, 22, 3, 22), true, 2400 + kComposerSpellcheckDebounceMs);
    check(fake.calls == calls + 1 && utf8_equal({fake.seen[calls], 3}, utf8_literal("xqa")) && spell.span_count == 2 &&
              spell.spans[1].start == 18 && spell.spans[1].end == 21,
          "spellcheck queries only the finished new word");

    // Moving the caret back into a word re-enables checking without a debounce.
    char moved[] = "hello xab";
    static ComposerSpellcheck fresh{};
    fresh = {};
    FakeChecker other{};
    fresh.backend = {&other, fake_check, fake_suggest, fake_learn};
    composer_spellcheck_tick(&fresh, document(moved, 9, 1, 9, 9), true, 10);
    composer_spellcheck_tick(&fresh, document(moved, 9, 2, 9, 9), true, 20);
    check(fresh.span_count == 0, "spellcheck hides the caret word after an edit");
    check(composer_spellcheck_tick(&fresh, document(moved, 9, 2, 3, 9), true, 21) && fresh.span_count == 1,
          "spellcheck marks the word once the caret moves away");

    // Per-tick budget: many unknown words drain over several ticks.
    char many[(kComposerSpellcheckTickBudget + 4) * 4];
    u32 bytes = 0;
    for (u32 i = 0; i < kComposerSpellcheckTickBudget + 4; ++i) {
        many[bytes++] = static_cast<char>('a' + i % 26);
        many[bytes++] = static_cast<char>('b' + i / 26);
        many[bytes++] = 'q';
        many[bytes++] = ' ';
    }
    static ComposerSpellcheck budget{};
    budget = {};
    FakeChecker counted{};
    budget.backend = {&counted, fake_check, fake_suggest, fake_learn};
    composer_spellcheck_tick(&budget, document(many, bytes, 1, bytes, 11), true, 100);
    check(counted.calls == kComposerSpellcheckTickBudget && composer_spellcheck_next_wake(budget) == 100 + kComposerSpellcheckContinueMs,
          "spellcheck bounds backend work per tick");
    composer_spellcheck_tick(&budget, document(many, bytes, 1, bytes, 11), true, 100 + kComposerSpellcheckContinueMs);
    check(counted.calls == kComposerSpellcheckTickBudget + 4 && !composer_spellcheck_next_wake(budget),
          "spellcheck finishes remaining words on later ticks");

    // Disabled: marks clear and the backend is never touched.
    const u32 before = fake.calls;
    check(composer_spellcheck_tick(&spell, document(text, 22, 4, 22), false, 5000) && spell.span_count == 0 &&
              fake.calls == before && !composer_spellcheck_next_wake(spell),
          "disabled spellcheck clears marks without backend calls");
    composer_spellcheck_tick(&spell, document(text, 22, 4, 22), true, 6000);
    check(spell.span_count == 2 && fake.calls == before, "re-enabled spellcheck restores marks from the cache");

    // Unavailable backend: no marks and no further attempts.
    static ComposerSpellcheck missing{};
    missing = {};
    FakeChecker absent{};
    absent.unavailable = true;
    missing.backend = {&absent, fake_check, fake_suggest, fake_learn};
    composer_spellcheck_tick(&missing, document(text, 22, 1, 22, 12), true, 1);
    composer_spellcheck_tick(&missing, document(text, 21, 2, 3, 12), true, 1000);
    check(missing.unavailable && absent.calls == 0 && missing.span_count == 0 && !composer_spellcheck_next_wake(missing),
          "unavailable spellcheck stops without marks");
}

// More unique words than the cache keeps (it restarts at 3/4 load): the byte
// frontier must still finish the draft with one backend query per word.
void test_frontier(Check check) noexcept {
    constexpr u32 kWords = 500;
    static char text[kWords * 4];
    u32 misspelled = 0;
    for (u32 i = 0; i < kWords; ++i) {
        text[i * 4] = static_cast<char>('a' + i % 26);
        text[i * 4 + 1] = static_cast<char>('a' + (i / 26) % 26);
        text[i * 4 + 2] = static_cast<char>('a' + i / 676);
        text[i * 4 + 3] = ' ';
        misspelled += text[i * 4] == 'x';
    }
    static ComposerSpellcheck spell{};
    spell = {};
    FakeChecker fake{};
    spell.backend = {&fake, fake_check, fake_suggest, fake_learn};
    u64 now = 100;
    u32 ticks = 0;
    composer_spellcheck_tick(&spell, document(text, sizeof(text), 1, sizeof(text), 21), true, now);
    while (composer_spellcheck_next_wake(spell) && ticks < 1000) {
        now = composer_spellcheck_next_wake(spell);
        composer_spellcheck_tick(&spell, document(text, sizeof(text), 1, sizeof(text), 21), true, now);
        ++ticks;
    }
    bool ordered = spell.span_count == misspelled;
    for (u32 i = 0; ordered && i < spell.span_count; ++i)
        ordered = spell.spans[i].end == spell.spans[i].start + 3 && text[spell.spans[i].start] == 'x' &&
                  (!i || spell.spans[i].start > spell.spans[i - 1].start);
    check(!composer_spellcheck_next_wake(spell) && fake.calls == kWords && ordered,
          "spellcheck frontier finishes past cache restarts with one query per word");
    check(!composer_spellcheck_tick(&spell, document(text, sizeof(text), 1, sizeof(text), 21), true, now + 1) &&
              fake.calls == kWords && spell.span_count == misspelled,
          "spellcheck frontier keeps evicted marks and verdicts while the text is unchanged");

    // An edit restarts the frontier: words whose verdicts were evicted are checked again.
    text[1] = 'z';
    composer_spellcheck_tick(&spell, document(text, sizeof(text), 2, 0, 21), true, now + 2);
    ticks = 0;
    while (composer_spellcheck_next_wake(spell) && ticks < 1000) {
        now = composer_spellcheck_next_wake(spell);
        composer_spellcheck_tick(&spell, document(text, sizeof(text), 2, 0, 21), true, now);
        ++ticks;
    }
    check(!composer_spellcheck_next_wake(spell) && fake.calls > kWords && fake.calls <= 2 * kWords + 1 &&
              spell.span_count == misspelled,
          "spellcheck frontier resets on edits and re-finishes with bounded queries");
}

void test_menu(Check check) noexcept {
    static ComposerSpellcheck spell{};
    spell = {};
    FakeChecker fake{};
    spell.backend = {&fake, fake_check, fake_suggest, fake_learn};
    const char text[] = "say helo now";
    const Utf8View view{text, 12};
    composer_spellcheck_tick(&spell, document(text, 12, 3, 12), true, 1);
    PopupItem items[kComposerSpellcheckMenuItems]{};
    check(!composer_spellcheck_menu_build(&spell, view, 3, 1, {items, kComposerSpellcheckMenuItems}),
          "spelling menu is empty over a correct word");
    const u32 count = composer_spellcheck_menu_build(&spell, view, 3, 8, {items, kComposerSpellcheckMenuItems});
    check(count == kComposerSpellcheckSuggestions + 2 && utf8_equal(items[0].label, utf8_literal("hello")) &&
              items[0].action == kComposerSpellcheckActionBase && (items[0].flags & popup_item_enabled) &&
              utf8_equal(items[5].label, utf8_literal("Add to dictionary")) &&
              (items[5].flags & popup_item_separator_before) && items[5].action == kComposerSpellcheckActionAdd &&
              utf8_equal(items[6].label, utf8_literal("Ignore")) && items[6].action == kComposerSpellcheckActionIgnore,
          "spelling menu lists five suggestions then dictionary actions");
    check(!composer_spellcheck_menu_action(static_cast<u32>(7)) &&
              composer_spellcheck_menu_action(kComposerSpellcheckActionIgnore),
          "spelling actions never collide with edit commands");
    check(composer_spellcheck_menu_activate(&spell, view, 4, kComposerSpellcheckActionBase).effect ==
              ComposerSpellcheckEffect::none,
          "stale spelling actions are rejected");
    composer_spellcheck_menu_build(&spell, view, 3, 4, {items, kComposerSpellcheckMenuItems});
    const auto replace = composer_spellcheck_menu_activate(&spell, view, 3, kComposerSpellcheckActionBase + 1);
    check(replace.effect == ComposerSpellcheckEffect::replace && replace.first == 4 && replace.last == 8 &&
              utf8_equal(replace.replacement, utf8_literal("help")),
          "spelling suggestion activation reports the word range and replacement");
    composer_spellcheck_menu_build(&spell, view, 3, 6, {items, kComposerSpellcheckMenuItems});
    const auto ignored = composer_spellcheck_menu_activate(&spell, view, 3, kComposerSpellcheckActionIgnore);
    check(ignored.effect == ComposerSpellcheckEffect::learned && fake.learned_ignores == 1 && !fake.learned_adds &&
              composer_spellcheck_next_wake(spell) == 1,
          "Ignore learns the word for the session and schedules a refresh");
    const u32 calls = fake.calls;
    check(composer_spellcheck_tick(&spell, document(text, 12, 3, 12), true, 2) && !spell.span_count && fake.calls == calls,
          "learned words lose their mark without a backend query");

    static ComposerSpellcheck bare{};
    bare = {};
    FakeChecker quiet{};
    bare.backend = {&quiet, fake_check, nullptr, fake_learn};
    composer_spellcheck_tick(&bare, document(text, 12, 1, 12, 8), true, 1);
    check(composer_spellcheck_menu_build(&bare, view, 1, 5, {items, kComposerSpellcheckMenuItems}) == 3 &&
              utf8_equal(items[0].label, utf8_literal("No suggestions")) && !(items[0].flags & popup_item_enabled),
          "spelling menu shows a disabled No suggestions row");
    composer_spellcheck_menu_activate(&bare, view, 1, kComposerSpellcheckActionAdd);
    check(quiet.learned_adds == 1, "Add to dictionary reaches the backend");
}
bool wide_equal(const wchar_t* left, const wchar_t* right) noexcept {
    while (*left && *left == *right)
        ++left, ++right;
    return *left == *right;
}
bool supports(void* context, const wchar_t* tag) noexcept {
    for (const wchar_t* const* list = static_cast<const wchar_t* const*>(context); *list; ++list)
        if (wide_equal(*list, tag))
            return true;
    return false;
}
void test_language(Check check) noexcept {
    wchar_t chosen[kSpellCheckerLanguageChars]{};
    const wchar_t* exact[] = {L"nl-NL", L"en-US", nullptr};
    const wchar_t* base[] = {L"de", L"en-US", nullptr};
    const wchar_t* fallback[] = {L"en-US", nullptr};
    const wchar_t* none[] = {L"fr-FR", nullptr};
    check(spell_checker_choose_language(L"nl-NL", supports, exact, {chosen, kSpellCheckerLanguageChars}) &&
              wide_equal(chosen, L"nl-NL"),
          "spell language prefers the user locale");
    check(spell_checker_choose_language(L"de-AT", supports, base, {chosen, kSpellCheckerLanguageChars}) &&
              wide_equal(chosen, L"de"),
          "spell language falls back to the base language");
    check(spell_checker_choose_language(L"nl-NL", supports, fallback, {chosen, kSpellCheckerLanguageChars}) &&
              wide_equal(chosen, L"en-US"),
          "spell language falls back to en-US");
    check(!spell_checker_choose_language(L"nl-NL", supports, none, {chosen, kSpellCheckerLanguageChars}) &&
              spell_checker_choose_language(nullptr, supports, fallback, {chosen, kSpellCheckerLanguageChars}),
          "spell language reports no supported language");
}
} // namespace

void test_composer_spellcheck(Check check) noexcept {
    test_language(check);
    test_tokenizer(check);
    test_cache(check);
    test_frontier(check);
    test_menu(check);
}

} // namespace maicord
