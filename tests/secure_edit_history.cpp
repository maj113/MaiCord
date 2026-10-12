#include "maicord/ui/secure_edit.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool equals(const SecureEdit& edit, Utf8View text) noexcept {
    const auto view = secure_edit_view(edit);
    return view.count == text.bytes && bytes_equal(view.data, text.data, text.bytes);
}
}
void test_secure_edit_history(void (*check)(bool, const char*) noexcept) noexcept {
    SecureEdit edit{};
    check(secure_edit_initialize(&edit, 32, false, {8, true}), "history field initializes");
    if (!edit.storage.data) return;
    check(!secure_edit_enable_history(&edit, 63) && !edit.history &&
        secure_edit_enable_history(&edit, 64) && !secure_edit_enable_history(&edit, 64), "history budget validated without replacing owner");
    check(secure_edit_assign(&edit, utf8_literal("ab")) == SecureEditResult::changed &&
        !secure_edit_can_undo(edit) && secure_edit_insert_scalar(&edit, 0x1F600) == SecureEditResult::changed,
        "history records Unicode typing after document load");
    const auto revision = edit.revision;
    check(secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("ab")) &&
        edit.anchor == 2 && edit.caret == 2 && edit.revision == revision + 1 && secure_edit_can_redo(edit),
        "history undo restores text and pre-edit caret");
    check(secure_edit_redo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("ab\xF0\x9F\x98\x80")) &&
        edit.anchor == 6 && edit.caret == 6 && !secure_edit_can_redo(edit), "history redo restores supplementary scalar");
    check(secure_edit_set_utf16_selection(&edit, {4, 1}) == SecureEditResult::changed &&
        secure_edit_insert_utf8(&edit, utf8_literal("X\n")) == SecureEditResult::changed &&
        equals(edit, utf8_literal("aX\n")) && secure_edit_undo(&edit) == SecureEditResult::changed &&
        edit.anchor == 6 && edit.caret == 1, "history replacement restores reversed selection");
    check(secure_edit_insert_scalar(&edit, '\t') == SecureEditResult::invalid_input && secure_edit_can_redo(edit) &&
        secure_edit_insert_utf8(&edit, utf8_literal("123456789")) == SecureEditResult::capacity && secure_edit_can_redo(edit),
        "history rejected input preserves redo and selection");
    check(secure_edit_insert_scalar(&edit, 'Q') == SecureEditResult::changed && equals(edit, utf8_literal("aQ")) &&
        !secure_edit_can_redo(edit), "history divergent edit drops redo");
    secure_edit_assign(&edit, utf8_literal("ab\xF0\x9F\x98\x80"));
    check(secure_edit_backspace(&edit) == SecureEditResult::changed && secure_edit_undo(&edit) == SecureEditResult::changed &&
        edit.anchor == 6 && edit.caret == 6 && equals(edit, utf8_literal("ab\xF0\x9F\x98\x80")),
        "history backspace restores collapsed original caret");
    secure_edit_move(&edit, SecureEditMove::home, false);
    check(secure_edit_delete_forward(&edit) == SecureEditResult::changed && secure_edit_undo(&edit) == SecureEditResult::changed &&
        edit.anchor == 0 && edit.caret == 0, "history forward delete restores collapsed original caret");
    const u16 replaced[] = {0xD83D, 0xDE03, '\r', '\n'};
    SecureEditUtf16Change change{};
    check(secure_edit_replace_utf16(&edit, 0, 2, {replaced, 4}, &change) == SecureEditResult::changed &&
        secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("ab\xF0\x9F\x98\x80")) &&
        secure_edit_redo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("\xF0\x9F\x98\x83\r\n\xF0\x9F\x98\x80")),
        "history native UTF16 replacement replays exact CRLF and Unicode");
    secure_edit_clear(&edit);
    check(!secure_edit_can_undo(edit) && !secure_edit_can_redo(edit) && !secure_edit_history_costs(edit).used_bytes,
        "history clear erases both directions");
    check(secure_edit_insert_utf16(&edit, 0xD83D) == SecureEditResult::pending_surrogate && !secure_edit_can_undo(edit) &&
        secure_edit_undo(&edit) == SecureEditResult::unchanged && !edit.pending_high_surrogate &&
        secure_edit_insert_utf16(&edit, 0xDE00) == SecureEditResult::invalid_input,
        "history undo clears an incomplete scalar without recording it");
    secure_edit_release(&edit);
    check(!edit.history && !edit.storage.data, "history release clears ownership");

    // A byte ring smaller than 64 replacements forces wrap and eviction without
    // relying on internal slots. Verify the externally visible sequence backward
    // and forward repeatedly, including full-field replacement at capacity.
    check(secure_edit_initialize(&edit, 32, false) && secure_edit_enable_history(&edit, 65), "history odd-sized ring initializes");
    if (!edit.history) { secure_edit_release(&edit); return; }
    char text[32]{}, previous[32]{};
    for (u32 round = 0; round < 100; ++round) {
        copy_bytes(previous, text, sizeof(text));
        for (u32 i = 0; i < 32; ++i) text[i] = static_cast<char>('A' + (round + i) % 26);
        secure_edit_select_all(&edit);
        check(secure_edit_insert_utf8(&edit, {text, 32}) == SecureEditResult::changed, "history full replacement fits ring");
        if (round) {
            check(secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, {previous, 32}) &&
                secure_edit_redo(&edit) == SecureEditResult::changed && equals(edit, {text, 32}),
                "history wrapped payload restores old and new full fields");
        }
    }
    check(secure_edit_history_costs(edit).undo_steps == 1 && secure_edit_history_costs(edit).used_bytes == 64,
        "history evicts oldest entries to honor byte budget");
    secure_edit_release(&edit);

    check(secure_edit_initialize(&edit, 256, false) && secure_edit_enable_history(&edit, 512), "history step-bound fixture initializes");
    if (edit.history) {
        for (u32 i = 0; i < 100; ++i) secure_edit_insert_scalar(&edit, 'a' + i % 26);
        const auto costs = secure_edit_history_costs(edit);
        check(costs.undo_steps == 64 && costs.used_bytes == 64, "history bounds record count independently of payload capacity");
        for (u32 i = 0; i < 64; ++i) check(secure_edit_undo(&edit) == SecureEditResult::changed, "history oldest retained step is undoable");
        check(edit.storage.bytes == 36 && secure_edit_undo(&edit) == SecureEditResult::unchanged, "history stops at eviction boundary");
        for (u32 i = 0; i < 64; ++i) check(secure_edit_redo(&edit) == SecureEditResult::changed, "history retained redo sequence");
        check(edit.storage.bytes == 100 && edit.storage.data[99] == 'a' + 99 % 26, "history redo returns complete text");
        secure_edit_assign(&edit, utf8_literal("new"));
        check(!secure_edit_can_undo(edit) && !secure_edit_can_redo(edit), "history document assignment resets lifetime");
    }
    secure_edit_release(&edit);
    check(secure_edit_initialize(&edit, 32, false) && secure_edit_enable_history(&edit, 64), "history grouping initializes");
    if (edit.history) {
        secure_edit_assign(&edit, utf8_literal("a\xF0\x9F\x98\x80z"));
        secure_edit_set_utf16_selection(&edit, {3, 1});
        check(secure_edit_begin_history_group(&edit) && !secure_edit_begin_history_group(&edit), "history groups reject nesting");
        const u16 first_candidate[] = {'x'}, final_candidate[] = {0xD83D, 0xDE03};
        secure_edit_replace_utf16(&edit, 1, 3, {first_candidate, 1}, &change);
        check(!secure_edit_can_undo(edit) && secure_edit_undo(&edit) == SecureEditResult::unchanged,
            "history active group blocks replay");
        secure_edit_replace_utf16(&edit, 1, 2, {final_candidate, 2}, &change);
        secure_edit_end_history_group(&edit);
        check(secure_edit_history_costs(edit).undo_steps == 1 && secure_edit_history_costs(edit).used_bytes == 8 &&
            secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("a\xF0\x9F\x98\x80z")) &&
            edit.anchor == 5 && edit.caret == 1 && secure_edit_redo(&edit) == SecureEditResult::changed &&
            equals(edit, utf8_literal("a\xF0\x9F\x98\x83z")), "history group records only final scalar-safe difference and original selection");
        secure_edit_undo(&edit);
        secure_edit_begin_history_group(&edit);
        secure_edit_replace_utf16(&edit, 1, 3, {final_candidate, 2}, &change);
        const u16 original_candidate[] = {0xD83D, 0xDE00};
        secure_edit_replace_utf16(&edit, 1, 3, {original_candidate, 2}, &change);
        secure_edit_end_history_group(&edit);
        check(secure_edit_can_redo(edit) && secure_edit_history_costs(edit).used_bytes == 8,
            "history composition canceled back to original text preserves redo");
        secure_edit_begin_history_group(&edit);
        for (u32 i = 0; i < 100; ++i) {
            const u16 candidate = i % 2 ? 'r' : 's';
            secure_edit_replace_utf16(&edit, 0, 1, {&candidate, 1}, &change);
        }
        secure_edit_end_history_group(&edit);
        check(!secure_edit_can_redo(edit) && secure_edit_history_costs(edit).undo_steps == 1 &&
            secure_edit_history_costs(edit).used_bytes == 2 && secure_edit_undo(&edit) == SecureEditResult::changed &&
            equals(edit, utf8_literal("a\xF0\x9F\x98\x80z")), "history many intermediate replacements occupy one bounded record");
        secure_edit_begin_history_group(&edit);
        secure_edit_select_all(&edit); secure_edit_insert_utf8(&edit, {});
        secure_edit_end_history_group(&edit);
        check(secure_edit_undo(&edit) == SecureEditResult::changed && equals(edit, utf8_literal("a\xF0\x9F\x98\x80z")),
            "history group restores a complete deletion");
        secure_edit_begin_history_group(&edit);
        secure_edit_assign(&edit, utf8_literal("reset"));
        secure_edit_end_history_group(&edit);
        check(!secure_edit_can_undo(edit) && equals(edit, utf8_literal("reset")), "history document replacement aborts group");
        secure_edit_assign(&edit, utf8_literal("a\xC2\xA0z"));
        secure_edit_begin_history_group(&edit);
        const u16 shared_suffix[] = {0x0120};
        secure_edit_replace_utf16(&edit, 1, 2, {shared_suffix, 1}, &change);
        secure_edit_end_history_group(&edit);
        check(secure_edit_history_costs(edit).used_bytes == 4 && secure_edit_undo(&edit) == SecureEditResult::changed &&
            equals(edit, utf8_literal("a\xC2\xA0z")) && secure_edit_redo(&edit) == SecureEditResult::changed &&
            equals(edit, utf8_literal("a\xC4\xA0z")), "history group expands shared continuation-byte suffix to scalar boundaries");
    }
    secure_edit_release(&edit);
    check(secure_edit_initialize(&edit, 32, true) && !secure_edit_enable_history(&edit, 64) && !edit.history &&
        !secure_edit_begin_history_group(&edit),
        "history password fields reject retained edit history");
    secure_edit_release(&edit);
}
}
