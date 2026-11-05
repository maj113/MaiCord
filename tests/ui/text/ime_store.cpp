// The text store of platforms whose input method talks through events (src/ui/text/linux/secure_text_store.cpp, over
// the headless window's text input log): what it tells the method about the field, and how preedit, commit and
// delete-surrounding events change the bound edit and the composition the widgets read.
#include "../../support/test_output.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/window/null.hpp"
#include "maicord/platform/window/wayland.hpp"
#include "maicord/ui/text/secure_text_store.hpp"

namespace maicord {
namespace ime_store_test {
namespace {

u32 failures;
u32 changes;
char window_marker;

void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}

void changed(void*) noexcept {
    ++changes;
}

// Each unit is eight pixels wide, from x = 10.
bool range_geometry(void*, u32 first, u32 last, SecureTextRect* rectangle, bool* clipped) noexcept {
    *rectangle = {static_cast<i32>(10 + first * 8), 20, static_cast<i32>(10 + last * 8), 40};
    *clipped = false;
    return true;
}

bool equals(const SecureEdit& edit, Utf8View text) noexcept {
    return edit.storage.bytes == text.bytes && bytes_equal(edit.storage.data, text.data, text.bytes);
}

bool surrounding_is(Utf8View text) noexcept {
    const NullWindowProbe& probe = null_window_probe();
    return probe.text_surrounding_bytes == text.bytes && bytes_equal(probe.text_surrounding, text.data, text.bytes);
}

WindowEvent ime(EventType type, const u16* text, u32 count, i32 begin = -1, i32 end = -1) noexcept {
    WindowEvent event{};
    event.type = type;
    event.ime.text = text;
    event.ime.count = count;
    event.ime.cursor_begin = begin;
    event.ime.cursor_end = end;
    return event;
}

WindowEvent delete_around(u32 before, u32 after) noexcept {
    WindowEvent event = ime(EventType::ime_delete_surrounding, nullptr, 0);
    event.ime.delete_before = before;
    event.ime.delete_after = after;
    return event;
}

void run() noexcept {
    null_window_reset();
    null_window_set_text_input_available(true);
    SecureEdit edit{};
    SecureTextStore store{};
    check(secure_edit_initialize(&edit, 256, false) && secure_edit_assign(&edit, utf8_literal("hello")) ==
                                                           SecureEditResult::changed,
          "ime store: the edit holds its text");
    check(secure_text_store_initialize(&store, {&window_marker, nullptr, changed, nullptr, range_geometry, nullptr}),
          "ime store: initializes");
    check(!secure_text_store_ime_event(ime(EventType::ime_commit, nullptr, 0)),
          "ime store: no field holds the text input before one is bound");

    // Binding and attaching tells the method about the field.
    check(secure_text_store_bind(&store, &edit, SecureTextInputScope::text) && secure_text_store_attach(&store),
          "ime store: a field binds and attaches");
    {
        const NullWindowProbe& probe = null_window_probe();
        check(probe.text_input_enabled && probe.text_input_enables == 1 && surrounding_is(utf8_literal("hello")) &&
                  probe.text_cursor_byte == 5 && probe.text_anchor_byte == 5 &&
                  probe.text_purpose == static_cast<u8>(TextInputPurpose::normal) && !probe.text_sensitive &&
                  probe.text_x == 50 && probe.text_y == 20 && probe.text_height == 20,
              "ime store: the method is told the text, the caret and where it is");
    }
    secure_text_store_synchronize(&store);
    check(null_window_probe().text_input_enables == 1, "ime store: unchanged state is not told again");

    // A preedit sits in the text between the caret and its end.
    static const u16 ni[] = {'n', 'i'};
    check(secure_text_store_ime_event(ime(EventType::ime_preedit, ni, 2, 2, 2)) && changes == 1 &&
              equals(edit, utf8_literal("helloni")) && edit.caret == 7 && secure_text_store_composing(store),
          "ime store: a preedit is shown in the text and notifies the owner");
    SecureTextComposition composition{};
    check(secure_text_store_composition(store, &composition) && composition.active && composition.start == 5 &&
              composition.end == 7 && composition.span_count == 1 && composition.spans[0].start == 5 &&
              composition.spans[0].end == 7 && composition.spans[0].attribute == TextCompositionAttribute::input,
          "ime store: one input span covers the preedit");
    check(surrounding_is(utf8_literal("hello")) && null_window_probe().text_cursor_byte == 5,
          "ime store: the preedit is not part of the surrounding text");

    // The method replaces the preedit and marks a converted segment.
    static const u16 candidate[] = {0x4F60};
    check(secure_text_store_ime_event(ime(EventType::ime_preedit, candidate, 1, 0, 1)) &&
              equals(edit, utf8_literal("hello\xE4\xBD\xA0")) && secure_text_store_composition(store, &composition) &&
              composition.end == 6 && composition.span_count == 1 &&
              composition.spans[0].attribute == TextCompositionAttribute::target_converted,
          "ime store: a new preedit replaces the old one with its converted segment");

    // Commit replaces the composition; the composition ends.
    static const u16 committed[] = {0x4F60, 0x597D};
    check(secure_text_store_ime_event(ime(EventType::ime_commit, committed, 2)) &&
              equals(edit, utf8_literal("hello\xE4\xBD\xA0\xE5\xA5\xBD")) && edit.caret == 11 && edit.anchor == 11 &&
              !secure_text_store_composing(store) && secure_text_store_composition(store, &composition) &&
              !composition.active && composition.span_count == 0,
          "ime store: a commit replaces the preedit and ends the composition");
    check(surrounding_is(utf8_literal("hello\xE4\xBD\xA0\xE5\xA5\xBD")) && null_window_probe().text_cursor_byte == 11,
          "ime store: the method is told the new text and caret");

    // A supplementary scalar is two units, committed without a composition.
    static const u16 emoji[] = {0xD83D, 0xDE00};
    check(secure_text_store_ime_event(ime(EventType::ime_commit, emoji, 2)) &&
              equals(edit, utf8_literal("hello\xE4\xBD\xA0\xE5\xA5\xBD\xF0\x9F\x98\x80")) && edit.caret == 15,
          "ime store: a commit without a preedit inserts at the caret");

    // The method removes text around the caret.
    check(secure_text_store_ime_event(delete_around(2, 0)) &&
              equals(edit, utf8_literal("hello\xE4\xBD\xA0\xE5\xA5\xBD")) && edit.caret == 11,
          "ime store: delete-surrounding removes units before the caret");
    check(secure_text_store_ime_event(delete_around(0, 0)) && equals(edit, utf8_literal("hello\xE4\xBD\xA0\xE5\xA5\xBD")),
          "ime store: deleting nothing changes nothing");
    check(!secure_text_store_ime_event(WindowEvent{}), "ime store: other events are not its own");

    // A selection is replaced by the first preedit.
    secure_edit_select_all(&edit);
    check(secure_text_store_ime_event(ime(EventType::ime_preedit, ni, 2, 2, 2)) && equals(edit, utf8_literal("ni")) &&
              secure_text_store_composing(store),
          "ime store: a preedit replaces the selection");
    // Unbinding while composing removes the preedit text.
    secure_text_store_unbind(&store);
    check(!secure_text_store_composing(store) && equals(edit, utf8_literal("")) && !null_window_probe().text_input_enabled,
          "ime store: unbinding discards the preedit and turns the method off");
    check(!secure_text_store_ime_event(ime(EventType::ime_commit, committed, 2)),
          "ime store: an unbound store takes no events");

    // A password edit refuses text operations: it takes no input method. A lost window focus turns the method off.
    secure_text_store_unbind(&store);
    secure_edit_release(&edit);
    check(secure_edit_initialize(&edit, 64, true) && secure_edit_assign(&edit, utf8_literal("secret")) ==
                                                         SecureEditResult::changed,
          "ime store: a password edit");
    check(secure_text_store_bind(&store, &edit, SecureTextInputScope::text) && secure_text_store_attach(&store) &&
              !null_window_probe().text_input_enabled && !secure_text_store_ime_event(ime(EventType::ime_commit, committed, 2)),
          "ime store: a password field gets no text input");
    secure_text_store_unbind(&store);
    secure_edit_release(&edit);
    check(secure_edit_initialize(&edit, 64, false) && secure_edit_assign(&edit, utf8_literal("12")) ==
                                                          SecureEditResult::changed &&
              secure_text_store_bind(&store, &edit, SecureTextInputScope::digits) && secure_text_store_attach(&store) &&
              null_window_probe().text_input_enabled &&
              null_window_probe().text_purpose == static_cast<u8>(TextInputPurpose::digits),
          "ime store: a digits field asks for a digits input");
    secure_text_store_set_window_focus(&store, false);
    check(!null_window_probe().text_input_enabled && !secure_text_store_ime_event(ime(EventType::ime_commit, committed, 2)),
          "ime store: a blurred window has no text input");
    secure_text_store_set_window_focus(&store, true);
    check(null_window_probe().text_input_enabled, "ime store: focus brings it back");
    secure_text_store_detach(&store);
    check(!null_window_probe().text_input_enabled, "ime store: detaching turns it off");

    // Without the protocol the store stays out of the way.
    null_window_set_text_input_available(false);
    check(secure_text_store_attach(&store) && !null_window_probe().text_input_enabled &&
              !secure_text_store_ime_event(ime(EventType::ime_commit, committed, 2)),
          "ime store: without text input support no event applies");

    secure_text_store_release(&store);
    secure_edit_release(&edit);
}

} // namespace
} // namespace ime_store_test
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_ime_store_entry) {
    maicord::ime_store_test::run();
    maicord::test_exit(maicord::ime_store_test::failures, "ime store");
}
