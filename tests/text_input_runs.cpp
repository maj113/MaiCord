#include "maicord/ui/secure_edit.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool equals(const SecureEdit& edit, Utf8View text) noexcept {
    return edit.storage.bytes == text.bytes && bytes_equal(edit.storage.data, text.data, text.bytes);
}
}
void test_text_input_runs(void (*check)(bool, const char*) noexcept) noexcept {
    SecureEdit edit{};
    check(secure_edit_initialize(&edit, 256, false, {128, true}) && secure_edit_enable_history(&edit, 512), "input run fixture initializes");
    if (!edit.history) { secure_edit_release(&edit); return; }
    for (u32 i = 0; i < 100; ++i) secure_edit_insert_scalar(&edit, 'a' + i % 26, i + 1);
    check(secure_edit_can_undo(edit) && secure_edit_undo(&edit) == SecureEditResult::changed && edit.storage.bytes == 0 &&
        !secure_edit_can_undo(edit) && secure_edit_redo(&edit) == SecureEditResult::changed && edit.storage.bytes == 100 &&
        edit.anchor == 100 && edit.caret == 100, "input run retains more than 64 typed scalars in one undo step");
    secure_edit_clear(&edit);
    secure_edit_insert_scalar(&edit, 'a', 100);
    secure_edit_insert_utf16(&edit, 0xD83D, 110);
    secure_edit_insert_utf16(&edit, 0xDE00, 120);
    check(secure_edit_undo(&edit) == SecureEditResult::changed && edit.storage.bytes == 0 &&
        secure_edit_redo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("a\xF0\x9F\x98\x80")),
        "input run coalesces a completed supplementary scalar");
    secure_edit_insert_scalar(&edit, 'b', 2000);
    secure_edit_insert_scalar(&edit, 'c', 3000);
    check(secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("a\xF0\x9F\x98\x80" "b")) &&
        secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("a\xF0\x9F\x98\x80")),
        "input run splits at a one-second gap");
    secure_edit_clear(&edit);
    secure_edit_insert_scalar(&edit, 'a', 500);
    secure_edit_insert_scalar(&edit, 'b', 499);
    check(secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("a")), "input run backwards timestamp starts a new unit");
    secure_edit_clear(&edit);
    secure_edit_insert_scalar(&edit, 'a', 10); secure_edit_insert_scalar(&edit, 'b', 11);
    secure_edit_insert_scalar(&edit, '\n', 12); secure_edit_insert_scalar(&edit, 'c', 13);
    check(secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("ab\n")) &&
        secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("ab")) &&
        secure_edit_undo(&edit) == SecureEditResult::changed && edit.storage.bytes == 0, "input run newline separates adjacent typing");
    secure_edit_insert_scalar(&edit, 'a', 20); secure_edit_insert_scalar(&edit, 'b', 21);
    secure_edit_backspace(&edit, 22);
    check(secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("ab")) && edit.anchor == 2 && edit.caret == 2 &&
        secure_edit_undo(&edit) == SecureEditResult::changed && edit.storage.bytes == 0 &&
        secure_edit_redo(&edit) == SecureEditResult::changed && edit.anchor == 2 && edit.caret == 2,
        "input run typing-to-deletion boundary preserves collapsed redo caret");
    secure_edit_assign(&edit, utf8_literal("a\xF0\x9F\x98\x80" "b"));
    secure_edit_backspace(&edit, 30); secure_edit_backspace(&edit, 31);
    check(secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("a\xF0\x9F\x98\x80" "b")) && edit.caret == 6 &&
        secure_edit_redo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("a")), "input run backspace restores Unicode deletion sequence");
    secure_edit_assign(&edit, utf8_literal("abcd")); secure_edit_move(&edit, SecureEditMove::home, false);
    secure_edit_delete_forward(&edit, 40); secure_edit_delete_forward(&edit, 41);
    check(secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("abcd")) && edit.anchor == 0 && edit.caret == 0 &&
        secure_edit_redo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("cd")), "input run forward deletion coalesces at stationary caret");
    secure_edit_clear(&edit);
    secure_edit_insert_scalar(&edit, 'a', 50); secure_edit_move(&edit, SecureEditMove::end, false);
    secure_edit_insert_scalar(&edit, 'b', 51);
    check(secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("a")), "input run navigation splits even at same position");
    secure_edit_insert_scalar(&edit, 'c', 52); secure_edit_insert_utf8(&edit, utf8_literal("paste"));
    check(secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("ac")) &&
        secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("a")), "input run paste remains separate and drops divergent redo");
    secure_edit_insert_scalar(&edit, 'b', 60);
    check(secure_edit_begin_history_group(&edit), "input run explicit native group ends ordinary typing first");
    secure_edit_insert_scalar(&edit, 'c', 61); secure_edit_insert_scalar(&edit, 'd', 62);
    secure_edit_end_history_group(&edit);
    check(secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("ab")) &&
        secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("a")), "input run and native transaction retain separate boundaries");
    secure_edit_insert_scalar(&edit, 'z', 70);
    secure_edit_break_history_run(&edit);
    secure_edit_insert_scalar(&edit, 'y', 71);
    check(secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("az")), "input run explicit focus barrier prevents merging");
    char full[127]{};
    for (auto& value : full) value = 'a';
    secure_edit_assign(&edit, {full, sizeof(full)});
    secure_edit_insert_scalar(&edit, 'z', 80);
    check(secure_edit_insert_scalar(&edit, 'x', 81) == SecureEditResult::capacity &&
        secure_edit_insert_scalar(&edit, '\t', 82) == SecureEditResult::invalid_input &&
        secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, {full, sizeof(full)}) && !secure_edit_can_undo(edit),
        "input run rejected operations preserve its original undo boundary");
    secure_edit_release(&edit);
}
}
