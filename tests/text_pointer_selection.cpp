#include "maicord/ui/text_pointer_selection.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/ui/secure_edit.hpp"
#include "maicord/core/utf.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace text_pointer_selection_test {
u32 failures{};
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool range(Utf8View text, u32 position, u32 first, u32 last, bool pointer = false) noexcept {
    u32 a = ~0u, b = ~0u;
    return (pointer ? utf8_word_range_near(text, position, &a, &b) : utf8_word_range(text, position, &a, &b)) &&
           a == first && b == last;
}
bool line(Utf8View text, u32 position, u32 first, u32 last) noexcept {
    u32 a = ~0u, b = ~0u;
    return utf8_line_range(text, position, &a, &b) && a == first && b == last;
}

void click_count() noexcept {
    constexpr UiClickPolicy policy{500, 2.f, 2.f};
    UiClickTracker t{};
    int owner = 0, other = 0;
    // Windows: DOWN, (UP), DBLCLK, (UP), DOWN for a triple click.
    check(ui_click_tracker_press(&t, &owner, 10, 10, 1000, policy, false) == 1 &&
              ui_click_tracker_press(&t, &owner, 10, 10, 1200, policy, true) == 2 &&
              ui_click_tracker_press(&t, &owner, 11, 9, 1400, policy, false) == 3,
          "click count: press, system double click and a near, timely press make a triple click");
    check(ui_click_tracker_press(&t, &owner, 11, 9, 1500, policy, false) == 1 &&
              ui_click_tracker_press(&t, &owner, 11, 9, 1600, policy, true) == 2,
          "click count: a fourth press restarts instead of escalating to select-all");
    check(ui_click_tracker_press(&t, &owner, 10, 10, 1000, policy, false) == 1 &&
              ui_click_tracker_press(&t, &owner, 10, 10, 1000, policy, false) == 1 &&
              ui_click_tracker_press(&t, &owner, 10, 10, 1000, policy, false) == 1,
          "click count: ordinary presses never escalate without a system double click");
    ui_click_tracker_press(&t, &owner, 10, 10, 1000, policy, true);
    check(ui_click_tracker_press(&t, &owner, 10, 10, 1501, policy, false) == 1,
          "click count: a third press after the double-click time is a new click");
    ui_click_tracker_press(&t, &owner, 10, 10, 1000, policy, true);
    check(ui_click_tracker_press(&t, &owner, 12.5f, 10, 1100, policy, false) == 1,
          "click count: a third press outside the double-click rectangle is a new click");
    ui_click_tracker_press(&t, &owner, 10, 10, 1000, policy, true);
    check(ui_click_tracker_press(&t, &other, 10, 10, 1100, policy, false) == 1,
          "click count: a press on another text surface is a new click");
    ui_click_tracker_press(&t, &owner, 10, 10, 1000, policy, true);
    ui_click_tracker_reset(&t);
    check(ui_click_tracker_press(&t, &owner, 10, 10, 1100, policy, false) == 1,
          "click count: keyboard input/focus loss resets the sequence");
    ui_click_tracker_press(&t, &owner, 10, 10, 0xFFFFFF00u, policy, true);
    check(ui_click_tracker_press(&t, &owner, 10, 10, 0x40u, policy, false) == 3,
          "click count: message-time wrap-around keeps the triple click");
    check(ui_text_selection_unit(1) == UiTextSelectionUnit::character &&
              ui_text_selection_unit(2) == UiTextSelectionUnit::word &&
              ui_text_selection_unit(3) == UiTextSelectionUnit::line,
          "click count maps to character, word and line units");
}

void word_boundaries() noexcept {
    constexpr auto words = utf8_literal("hello world");
    check(range(words, 2, 0, 5) && range(words, 5, 5, 6) && range(words, 5, 0, 5, true) &&
              range(words, 6, 6, 11, true) && range(words, 11, 6, 11) && range(words, 11, 6, 11, true),
          "word range: letters, space run and the rounded trailing half of a word's last letter");
    constexpr auto punctuated = utf8_literal("hello, world!");
    check(range(punctuated, 5, 5, 6) && range(punctuated, 6, 5, 6, true) && range(punctuated, 12, 12, 13) &&
              range(punctuated, 13, 12, 13, true) && range(utf8_literal("snake_case2 x"), 3, 0, 11),
          "word range: punctuation is its own run; underscore and digits join words");
    constexpr auto nbsp = utf8_literal("foo\xC2\xA0" "bar\xE3\x80\x80" "baz");
    check(range(nbsp, 1, 0, 3) && range(nbsp, 3, 3, 5) && range(nbsp, 6, 5, 8) && range(nbsp, 11, 11, 14),
          "word range: no-break and ideographic spaces separate words");
    constexpr auto cjk = utf8_literal("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xA7\xE3\x81\x99"
                                      "\xE3\x82\xAB\xE3\x83\xA1\xE3\x83\xA9\xE3\x80\x82");
    check(range(cjk, 3, 0, 9) && range(cjk, 9, 9, 15) && range(cjk, 18, 15, 24) && range(cjk, 24, 24, 27),
          "word range: Han, Hiragana and Katakana runs and CJK punctuation stay separate");
    constexpr auto emoji = utf8_literal("hi\xF0\x9F\x99\x82\xF0\x9F\x99\x82there");
    check(range(emoji, 0, 0, 2) && range(emoji, 2, 2, 6) && range(emoji, 6, 6, 10) && range(emoji, 10, 10, 15),
          "word range: each emoji is its own word and never joins adjacent letters");
    constexpr auto clusters = utf8_literal("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB"
                                           "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"
                                           "\xF0\x9F\x87\xB8\xF0\x9F\x87\xAE\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8");
    check(range(clusters, 0, 0, 11) && range(clusters, 7, 0, 11) && range(clusters, 11, 11, 19) &&
              range(clusters, 15, 11, 19) && range(clusters, 19, 19, 27) && range(clusters, 27, 27, 35),
          "word range: ZWJ sequences, skin-tone modifiers and flag pairs are single emoji clusters");
    constexpr auto marks = utf8_literal("cafe\xCC\x81 x");
    check(range(marks, 0, 0, 6) && range(marks, 4, 0, 6) && range(marks, 6, 6, 7, false),
          "word range: combining marks extend their base letter");
    constexpr auto lines = utf8_literal("ab\n\ncd  \nef");
    check(range(lines, 2, 0, 2) && range(lines, 3, 3, 3) && range(lines, 4, 4, 6) && range(lines, 8, 6, 8) &&
              range(lines, 6, 6, 8) && range(lines, 9, 9, 11),
          "word range: a line break selects its line's last run, an empty line nothing, never across lines");
    check(line(lines, 0, 0, 2) && line(lines, 2, 0, 2) && line(lines, 3, 3, 3) && line(lines, 5, 4, 8) &&
              line(lines, 11, 9, 11) && line(utf8_literal(""), 0, 0, 0),
          "line range: triple click selects the paragraph without its break");
    u32 boundary = 0;
    check(utf8_word_boundary(utf8_literal("ab cd"), 0, true, &boundary) && boundary == 3 &&
              utf8_word_boundary(utf8_literal("ab\ncd"), 0, true, &boundary) && boundary == 3 &&
              utf8_word_boundary(utf8_literal("ab\ncd"), 5, false, &boundary) && boundary == 3,
          "word navigation keeps treating line breaks as spaces");
    u32 a = 0, b = 0;
    check(!utf8_word_range(utf8_literal("\xF0\x9F\x99\x82"), 1, &a, &b) &&
              !utf8_word_range(utf8_literal("ab"), 3, &a, &b) && !utf8_line_range(utf8_literal("ab"), 3, &a, &b),
          "word range rejects positions inside a scalar or past the end");
}

void pointer_selection() noexcept {
    constexpr auto text = utf8_literal("alpha beta gamma\ndelta");
    UiTextPointerSelection state{};
    u32 anchor = 0, caret = 0;
    check(ui_text_pointer_press(text, 8, 2, false, false, 0, false, &state, &anchor, &caret) && anchor == 6 &&
              caret == 10 && state.unit == UiTextSelectionUnit::word,
          "pointer: double click selects the word under the pointer");
    check(ui_text_pointer_press(text, 10, 2, false, false, 0, false, &state, &anchor, &caret) && anchor == 6 &&
              caret == 10,
          "pointer: double click on the trailing half of a word's last letter still selects that word");
    check(ui_text_pointer_drag(text, state, 9, false, false, &anchor, &caret) && anchor == 6 && caret == 10 &&
              ui_text_pointer_drag(text, state, 10, false, false, &anchor, &caret) && anchor == 6 && caret == 10,
          "pointer: jitter inside the double-clicked word keeps the whole word");
    check(ui_text_pointer_drag(text, state, 13, false, false, &anchor, &caret) && anchor == 6 && caret == 16 &&
              ui_text_pointer_drag(text, state, 1, false, false, &anchor, &caret) && anchor == 10 && caret == 0,
          "pointer: dragging after a double click extends by whole words in both directions");
    check(ui_text_pointer_drag(text, state, 19, false, false, &anchor, &caret) && anchor == 6 && caret == 22,
          "pointer: word drag continues onto the next line by whole words");
    check(ui_text_pointer_press(text, 8, 3, false, false, 0, false, &state, &anchor, &caret) && anchor == 0 &&
              caret == 16 && ui_text_pointer_drag(text, state, 20, false, false, &anchor, &caret) && anchor == 0 &&
              caret == 22,
          "pointer: triple click selects the paragraph and drags by paragraphs");
    check(ui_text_pointer_press(text, 8, 3, true, false, 0, false, &state, &anchor, &caret) && anchor == 0 &&
              caret == text.bytes,
          "pointer: triple click in a single-line field selects all of it");
    check(ui_text_pointer_press(text, 8, 1, false, true, 2, false, &state, &anchor, &caret) && anchor == 2 &&
              caret == 8 && ui_text_pointer_drag(text, state, 12, false, false, &anchor, &caret) && anchor == 2 &&
              caret == 12,
          "pointer: shift-click extends from the anchor and drags by characters");
    const auto before = state;
    anchor = caret = 77;
    check(!ui_text_pointer_press(text, 8, 2, false, false, 0, true, &state, &anchor, &caret) && anchor == 77 &&
              caret == 77 && state.origin_first == before.origin_first &&
              !ui_text_pointer_drag(text, state, 12, false, true, &anchor, &caret) && anchor == 77,
          "pointer: an active IME composition blocks pointer selection changes");
    check(!ui_text_pointer_press(text, 99, 2, false, false, 0, false, &state, &anchor, &caret) &&
              !ui_text_pointer_drag(text, {0, 99, UiTextSelectionUnit::word}, 1, false, false, &anchor, &caret),
          "pointer: out-of-range positions are rejected");
}

void edit_models() noexcept {
    char storage[64]{};
    MessageComposer composer{};
    check(message_composer_initialize(&composer, {storage, 64}) &&
              message_composer_insert_utf8(&composer, utf8_literal("hi <@42> there")) ==
                  MessageComposerResult::changed,
          "composer model initializes with a mention token");
    const MessageComposerAtomicSpan spans[]{{3, 8}};
    composer.atomic_spans = {spans, 1};
    composer.atomic_revision = composer.text_revision;
    check(message_composer_select_word(&composer, 3) != MessageComposerResult::invalid_input &&
              composer.anchor == 3 && composer.caret == 8,
          "composer: double click on a mention's start selects the whole token");
    check(message_composer_select_word(&composer, 8) != MessageComposerResult::invalid_input &&
              composer.anchor == 3 && composer.caret == 8,
          "composer: double click on a mention's trailing half selects the whole token");
    check(message_composer_select_range(&composer, 10, 5) != MessageComposerResult::invalid_input &&
              composer.anchor == 10 && composer.caret == 3 &&
              message_composer_select_range(&composer, 0, 99) == MessageComposerResult::invalid_input,
          "composer: word drags widen to whole tokens in either direction");
    check(message_composer_select_word(&composer, 11) != MessageComposerResult::invalid_input &&
              composer.anchor == 9 && composer.caret == 14,
          "composer: ordinary words are unaffected by tokens");
    SecureEdit edit{};
    check(secure_edit_initialize(&edit, 64, false, {0, true}) &&
              secure_edit_insert_utf8(&edit, utf8_literal("first line\nsecond")) == SecureEditResult::changed &&
              secure_edit_select_line(&edit, 3) == SecureEditResult::changed && edit.anchor == 0 &&
              edit.caret == 10 && secure_edit_select_line(&edit, 14) == SecureEditResult::changed &&
              edit.anchor == 11 && edit.caret == 17 && secure_edit_select_word(&edit, 5) == SecureEditResult::changed &&
              edit.anchor == 0 && edit.caret == 5,
          "secure edit: triple click selects the line and double click the word near the pointer");
    secure_edit_release(&edit);
}

void run() noexcept {
    click_count();
    word_boundaries();
    pointer_selection();
    edit_models();
}
} // namespace text_pointer_selection_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_text_pointer_selection_entry() noexcept {
    maicord::text_pointer_selection_test::run();
    const bool failed = maicord::text_pointer_selection_test::failures != 0;
    const auto result = failed ? maicord::utf8_literal("FAIL text pointer selection\n")
                               : maicord::utf8_literal("PASS text pointer selection\n");
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), result.data, result.bytes, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
