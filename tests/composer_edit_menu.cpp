#include "../src/app/application_composer_edit_menu.hpp"
#include "../src/app/application_state.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/message_draft_store.hpp"
#include "maicord/protocol/message_create.hpp"
#include "maicord/protocol/message_edit.hpp"
#include "maicord/ui/popup_presentation.hpp"
namespace maicord::app_detail {
namespace { bool composing; u32 applied; }
bool application_composer_text_input_composing() noexcept { return composing; }
void end_message_composer_pointer_selection() noexcept {}
bool focus_message_composer_control(MessageComposerControl control) noexcept {
    if (!message_composer_scene_focus_control(&g_app.message_composer_scene, control)) return false;
    g_app.message_composer.focused = control == MessageComposerControl::edit;
    ++g_app.message_composer.revision;
    return true;
}
bool composer_apply_result(MessageComposerResult result, bool) noexcept {
    if (result == MessageComposerResult::changed) ++applied;
    return result == MessageComposerResult::changed || result == MessageComposerResult::unchanged || result == MessageComposerResult::capacity;
}
bool dismiss_popup() noexcept { popup_scene_close(&g_app.popup_scene); g_app.popup_owner_kind = PopupOwnerKind::none; return true; }
}
namespace maicord {
namespace {
struct Clipboard { char text[128]; u32 bytes, reads, writes; bool fail_read, fail_write; };
ClipboardTextResult read(void* opaque, Slice<char> output) noexcept {
    auto& clipboard = *static_cast<Clipboard*>(opaque); ++clipboard.reads;
    if (clipboard.fail_read) return {0, ClipboardTextError::open_failed};
    if (clipboard.bytes > output.count) return {0, ClipboardTextError::capacity};
    copy_bytes(output.data, clipboard.text, clipboard.bytes); return {clipboard.bytes, ClipboardTextError::none};
}
bool write(void* opaque, Utf8View text) noexcept {
    auto& clipboard = *static_cast<Clipboard*>(opaque); ++clipboard.writes;
    if (clipboard.fail_write || text.bytes > sizeof(clipboard.text)) return false;
    copy_bytes(clipboard.text, text.data, text.bytes); clipboard.bytes = text.bytes; return true;
}
bool equal(Utf8View a, Utf8View b) noexcept { return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes); }
ComposerSpellVerdict spell_check(void*, Utf8View word) noexcept {
    return equal(word, utf8_literal("helo")) ? ComposerSpellVerdict::misspelled : ComposerSpellVerdict::correct;
}
u32 spell_suggest(void*, Utf8View, Slice<char> storage, Slice<Utf8View> output) noexcept {
    if (storage.count < 5 || !output.count) return 0;
    copy_bytes(storage.data, "hello", 5); output[0] = {storage.data, 5}; return 1;
}
void set_clipboard(Clipboard& clipboard, Utf8View text) noexcept {
    copy_bytes(clipboard.text, text.data, text.bytes); clipboard.bytes = text.bytes; clipboard.fail_read = false;
}
// Colored-text generators copy a ```ansi fence holding real U+001B escapes;
// the official composer inserts them verbatim and sends them as \u001b.
void test_composer_ansi_paste(void (*check)(bool, const char*) noexcept, MessageComposer& composer,
                              Clipboard& clipboard, const app_detail::ComposerEditClipboard& provider) noexcept {
    using namespace app_detail;
    constexpr Utf8View expected = utf8_literal("```ansi\n\x1b[31mRed\x1b[0m\tx\n```");
    message_composer_restore(&composer, 7, utf8_literal(""), 0, 0);
    message_composer_enable_history(&composer);
    set_clipboard(clipboard, utf8_literal("```ansi\r\n\x1b[31mRed\x1b[0m\tx\r\n```"));
    check(application_composer_edit_command(TextEditCommand::paste, &provider) &&
        equal(message_composer_text(composer), expected) && composer.caret == expected.bytes &&
        message_composer_codepoints(composer) == 26,
        "composer Paste keeps ANSI escapes and tabs while normalizing CRLF");
    message_composer_set_selection(&composer, 8, 8);
    const bool forward = message_composer_move(&composer, MessageComposerMove::next, false) ==
        MessageComposerResult::changed && composer.caret == 9;
    const bool back = message_composer_move(&composer, MessageComposerMove::previous, true) ==
        MessageComposerResult::changed && composer.anchor == 9 && composer.caret == 8;
    message_composer_set_selection(&composer, 17, 17);
    const bool erased = message_composer_backspace(&composer) == MessageComposerResult::changed &&
        equal(message_composer_text(composer), utf8_literal("```ansi\n\x1b[31mRed[0m\tx\n```")) && composer.caret == 16;
    const bool restored = message_composer_undo(&composer) == MessageComposerResult::changed &&
        equal(message_composer_text(composer), expected);
    check(forward && back && erased && restored, "composer caret, selection and Backspace treat ESC as one character");
    const auto revision = composer.text_revision;
    message_composer_set_selection(&composer, 0, 0);
    set_clipboard(clipboard, {"a\0b", 3});
    const bool nul_refused = !application_composer_edit_command(TextEditCommand::paste, &provider) &&
        composer.text_revision == revision;
    const bool typed_escape = message_composer_insert_utf16(&composer, 0x1b) == MessageComposerResult::invalid_input &&
        message_composer_insert_utf16(&composer, 0x7f) == MessageComposerResult::invalid_input;
    check(nul_refused && typed_escape && composer.text_revision == revision,
        "composer refuses NUL text and keeps typed Escape and Ctrl+Backspace as commands");
    // IME/TSF replacement shares the inserted-text policy.
    const u16 ime[]{0x1b, '[', '1', 'm'};
    TextInputChange change{};
    check(text_input_replace(message_composer_input_target(&composer), 0, 0, {ime, 4}, &change) == TextInputResult::changed &&
        composer.bytes == expected.bytes + 4 && bytes_equal(composer.text, "\x1b[1m```", 7) &&
        message_composer_undo(&composer) == MessageComposerResult::changed && equal(message_composer_text(composer), expected),
        "composer text input target accepts ESC from native text services");
    MessageDraftRecord records[4]{}; u8 storage[128]{}; MessageDraftStore drafts{};
    MessageDraftSnapshot snapshot{};
    const bool drafted = message_draft_store_initialize(&drafts, {records, 4}, {storage, sizeof(storage)}) &&
        message_draft_store_save(&drafts, {7, message_composer_text(composer), 9, 9}).ok() &&
        message_composer_restore(&composer, 7, utf8_literal(""), 0, 0) &&
        message_draft_store_snapshot(drafts, 7, &snapshot) &&
        message_composer_restore(&composer, 7, snapshot.text, snapshot.anchor, snapshot.caret) &&
        equal(message_composer_text(composer), expected) && composer.caret == 9;
    check(drafted, "composer draft round-trip preserves ANSI escapes and the caret after one");
    u8 body[256]{};
    const auto created = write_message_create_body(message_composer_text(composer), 99, Slice<u8>{body, sizeof(body)});
    constexpr Utf8View created_json = utf8_literal("{\"content\":\"```ansi\\n\\u001B[31mRed\\u001B[0m\\tx\\n```\",\"nonce\":\"99\","
        "\"tts\":false,\"flags\":0,\"mobile_network_type\":\"unknown\",\"enforce_nonce\":true}");
    const bool create_ok = created.ok() && created.bytes == created_json.bytes &&
        bytes_equal(body, created_json.data, created_json.bytes);
    const auto edited = write_message_edit_body(message_composer_text(composer), Slice<u8>{body, sizeof(body)});
    constexpr Utf8View edited_json = utf8_literal("{\"content\":\"```ansi\\n\\u001B[31mRed\\u001B[0m\\tx\\n```\"}");
    check(create_ok && edited.ok() && edited.bytes == edited_json.bytes && bytes_equal(body, edited_json.data, edited_json.bytes),
        "send and edit bodies encode ESC as a JSON \\u001B escape");
}
}
void test_composer_edit_menu(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace app_detail;
    PopupItem items[kTextEditMenuItems]{};
    TextEditMenuState capabilities{true, true, true, true, true, true, true, false};
    check(text_edit_menu_build(capabilities, {items, kTextEditMenuItems}) && items[3].shortcut == 4 &&
        (items[2].flags & popup_item_separator_before), "shared edit menu supplies command hints and groups");
    capabilities.composing = true;
    check(!text_edit_command_enabled(TextEditCommand::paste, capabilities) &&
        !text_edit_command_enabled(TextEditCommand::undo, capabilities) &&
        text_edit_command_enabled(TextEditCommand::copy, capabilities), "shared menu protects composition from mutations");
    capabilities.composing = false; capabilities.copy_allowed = false;
    check(!text_edit_command_enabled(TextEditCommand::copy, capabilities) &&
        !text_edit_command_enabled(TextEditCommand::cut, capabilities), "shared edit menu supports protected-field copy policy");

    auto* account = static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    char text[64]{};
    UiWidget widgets[40]{}; UiLayoutNode nodes[40]{};
    bool ready = account && message_composer_initialize(&g_app.message_composer, {text, sizeof(text)}) &&
        message_composer_scene_initialize_owned(&g_app.message_composer_scene) &&
        popup_scene_initialize_owned(&g_app.popup_scene, 16, {widgets, 40}, {nodes, 40});
    check(ready, "composer edit-menu application fixture initializes");
    g_app.account_ui = account; g_app.window = reinterpret_cast<HWND>(1);
    if (ready) {
        auto& composer = g_app.message_composer;
        message_composer_reset(&composer, 7, false);
        message_composer_enable_history(&composer);
        message_composer_insert_utf8(&composer, utf8_literal("a\xF0\x9F\x98\x80z"));
        message_composer_set_selection(&composer, 5, 1);
        message_composer_scene_update(&g_app.message_composer_scene, &composer, {0, 0, 600, 80}, true);
        Clipboard clipboard{}; ComposerEditClipboard provider{&clipboard, read, write, true};
        check(application_composer_edit_command(TextEditCommand::copy, &provider) && clipboard.bytes == 4 && composer.bytes == 6 &&
            composer.anchor == 5 && composer.caret == 1, "composer Copy preserves a reversed supplementary-character selection");
        clipboard.fail_write = true;
        check(application_composer_edit_command(TextEditCommand::cut, &provider) && composer.bytes == 6 &&
            composer.anchor == 5 && composer.caret == 1, "failed clipboard write cannot delete a Cut selection");
        clipboard.fail_write = false;
        check(application_composer_edit_command(TextEditCommand::cut, &provider) && equal(message_composer_text(composer), utf8_literal("az")) &&
            application_composer_edit_command(TextEditCommand::undo, &provider) && composer.bytes == 6 && composer.anchor == 5 && composer.caret == 1,
            "composer Cut is reversible with its original selection");
        const char pasted[] = "x\r\ny\rz";
        copy_bytes(clipboard.text, pasted, sizeof(pasted) - 1); clipboard.bytes = sizeof(pasted) - 1;
        check(application_composer_edit_command(TextEditCommand::paste, &provider) &&
            equal(message_composer_text(composer), utf8_literal("ax\ny\nzz")), "composer Paste normalizes CRLF and CR through the shared command path");
        const auto revision = composer.revision; clipboard.fail_read = true;
        check(application_composer_edit_command(TextEditCommand::paste, &provider) && composer.revision == revision,
            "clipboard read failure preserves the draft");
        composing = true;
        const auto reads = clipboard.reads;
        check(application_composer_edit_command(TextEditCommand::paste, &provider) && clipboard.reads == reads && composer.revision == revision,
            "composer commands leave active native composition alone");
        composing = false;
        application_composer_edit_command(TextEditCommand::select_all, &provider);
        const u32 selected_anchor = composer.anchor, selected_caret = composer.caret;
        check(application_composer_edit_menu_open({590, 70, 591, 71}) && composer.anchor == selected_anchor && composer.caret == selected_caret &&
            g_app.popup_scene.item_count == 7 && g_app.popup_scene.owner_id == composer.document_identity,
            "composer context menu preserves selection and captures document identity");
        UiTextCommand commands[16]{}; UiTextCommandStream stream{};
        UiTextOutput stream_output{&stream, nullptr};
        ui_text_command_stream_initialize(&stream, {commands, 16});
        check(popup_text_presentation_build_output(g_app.popup_scene, kDefaultTheme, &stream_output) && stream.count == 14 &&
            commands[0].horizontal_alignment == UiTextAlignment::end && commands[0].rectangle.left > commands[1].rectangle.right,
            "edit menu shortcut and label columns have separate bounded text geometry");
        message_composer_restore(&composer, 7, utf8_literal("replacement"), 0, 11);
        check(application_composer_edit_menu_action(static_cast<u32>(TextEditCommand::erase)) &&
            equal(message_composer_text(composer), utf8_literal("replacement")) && !g_app.popup_scene.active,
            "stale context-menu action cannot alter a replacement document");
        message_composer_scene_update(&g_app.message_composer_scene, &composer, {0, 0, 600, 80}, true);
        check(application_composer_edit_menu_open({10, 20, 11, 21}) &&
            application_composer_edit_menu_action(static_cast<u32>(TextEditCommand::erase)) && !composer.bytes && !g_app.popup_scene.active,
            "current context-menu Delete uses the application edit command");
        message_composer_insert_utf8(&composer, utf8_literal("next"));
        message_composer_scene_update(&g_app.message_composer_scene, &composer, {0, 0, 600, 80}, true);
        check(application_composer_edit_menu_open({10, 20, 11, 21}), "context menu reopens for a fresh draft");
        message_composer_set_selection(&composer, 0, 0);
        check(application_composer_edit_menu_synchronize() && !g_app.popup_scene.active && composer.bytes == 4,
            "presentation dismisses a menu when its captured selection changes");
        // Spelling suggestions lead the menu; applying one is a single undoable edit.
        auto& spell = g_app.composer_spellcheck;
        spell = {};
        spell.backend = {nullptr, spell_check, spell_suggest, nullptr};
        message_composer_restore(&composer, 7, utf8_literal("say helo"), 8, 8);
        message_composer_enable_history(&composer);
        message_composer_scene_update(&g_app.message_composer_scene, &composer, {0, 0, 600, 80}, true);
        composer_spellcheck_tick(&spell, {&composer, composer.document_identity, composer.text_revision, composer.caret,
            message_composer_text(composer)}, true, 1);
        check(spell.span_count == 1 && application_composer_edit_menu_open({10, 20, 11, 21}, 1) &&
            g_app.popup_scene.item_count == 7 && dismiss_popup(), "composer menu over a correct word keeps the edit commands");
        check(application_composer_edit_menu_open({10, 20, 11, 21}, 5) && g_app.popup_scene.item_count == 10 &&
            g_app.popup_scene.items[0].action == kComposerSpellcheckActionBase &&
            (g_app.popup_scene.items[3].flags & popup_item_separator_before), "composer menu leads with spelling suggestions");
        check(application_composer_edit_menu_action(kComposerSpellcheckActionBase) && !g_app.popup_scene.active &&
            equal(message_composer_text(composer), utf8_literal("say hello")), "spelling suggestion replaces the flagged word");
        check(application_composer_edit_command(TextEditCommand::undo) &&
            equal(message_composer_text(composer), utf8_literal("say helo")), "spelling replacement undoes as one edit");
        spell = {};
        test_composer_ansi_paste(check, composer, clipboard, provider);
    }
    popup_scene_release(&g_app.popup_scene); message_composer_scene_release(&g_app.message_composer_scene);
    message_composer_release(&g_app.message_composer);
    if (account) VirtualFree(account, 0, MEM_RELEASE);
    g_app.account_ui = nullptr; g_app.window = nullptr; g_app.popup_owner_kind = PopupOwnerKind::none;
    g_app.composer_menu_revision = 0; g_app.composer_menu_held_key = 0; composing = false;
}
}
