#include "maicord/app/message_composer_controller.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/message_composer_scene.hpp"
namespace maicord {
namespace {
bool equal(const MessageComposer& composer, Utf8View text) noexcept {
    return composer.bytes == text.bytes && bytes_equal(composer.text, text.data, text.bytes);
}
}
void test_composer_history(void (*check)(bool, const char*) noexcept) noexcept {
    {
        MessageComposer composer{};
        char buffer[64]{};
        message_composer_initialize(&composer, {buffer, sizeof(buffer)});
        check(text_input_ready(message_composer_input_target(&composer)), "native text target supports editing before a DM channel exists");
        message_composer_reset(&composer, 7, false);
        message_composer_insert_utf8(&composer, utf8_literal("a\xF0\x9F\x98\x80\n\xE3\x81\x8B"));
        const auto target = message_composer_input_target(&composer);
        u32 length = 0, count = 0, next = 0;
        u16 copied[8]{};
        check(text_input_length(target, &length) == TextInputResult::unchanged && length == 5 &&
            text_input_copy(target, 0, 5, {copied, 2}, &count, &next) == TextInputResult::unchanged && count == 1 && next == 1,
            "composer ACP copy never splits a surrogate pair at capacity");
        const auto revision = text_input_revision(target);
        check(text_input_select(target, {5, 1}) == TextInputResult::changed && composer.anchor == 9 && composer.caret == 1 &&
            text_input_revision(target) == revision && text_input_select(target, {2, 2}) == TextInputResult::invalid_utf16,
            "composer native selection is scalar aligned and does not notify text changes");
        const u16 invalid[] = {0xD800}, replacement[] = {0x3042, 0xD83D, 0xDE00};
        TextInputChange change{};
        check(text_input_replace(target, 1, 3, {invalid, 1}, &change) == TextInputResult::invalid_utf16 &&
            text_input_revision(target) == revision && composer.anchor == 9 && composer.caret == 1,
            "composer malformed native replacement preserves text and selection");
        check(text_input_replace(target, 1, 3, {replacement, 3}, &change) == TextInputResult::changed &&
            change.start == 1 && change.old_end == 3 && change.new_end == 4 && text_input_revision(target) != revision,
            "composer native replacement reports UTF16 change coordinates");
        message_composer_restore(&composer, 7, utf8_literal("draft"), 5, 5);
        check(!text_input_ready(target) && text_input_replace(target, 0, 1, {replacement, 3}, &change) == TextInputResult::invalid_input &&
            equal(composer, utf8_literal("draft")), "same-channel restoration invalidates a stale native document");
        auto live = message_composer_input_target(&composer);
        message_composer_reset(&composer, 7, true);
        check(text_input_ready(live), "retaining the same draft keeps its native identity");
        message_composer_reset(&composer, 8, true);
        check(!text_input_ready(live), "channel transition invalidates native identity even when retaining text");
        message_composer_clear(&composer);
        live = message_composer_input_target(&composer);
        message_composer_insert_utf16(&composer, 0xD83D);
        message_composer_clear(&composer);
        check(!text_input_ready(live) && !composer.pending_high_surrogate, "empty send boundary discards native identity and incomplete surrogate");

        // Real projection replaces six source bytes with four visible bytes.
        message_composer_restore(&composer, 8, utf8_literal("<@123>\xF0\x9F\x98\x80\xE3\x81\x8B"), 13, 13);
        char display[] = "@Mai\xF0\x9F\x98\x80\xE3\x81\x8B";
        MessageComposerProjectionSpan mention{0, 6, 0, 4};
        MessageComposerScene scene{};
        scene.model = &composer; scene.display = display; scene.display_bytes = sizeof(display) - 1;
        scene.display_source_last = composer.bytes; scene.projection_spans = &mention; scene.projection_span_count = 1;
        TextCompositionSpan span{}; span.start = 6; span.end = 9;
        u32 first = 0, last = 0;
        check(message_composer_scene_composition_range(scene, span, &first, &last) && first == 4 && last == 7,
            "native composition maps Unicode ACP through a rendered mention");
        span.start = 7;
        check(!message_composer_scene_composition_range(scene, span, &first, &last), "native composition rejects half-surrogate geometry");
        message_composer_release(&composer);
    }
    MessageComposer owned{};
    check(message_composer_initialize_owned(&owned, 256) && !owned.history, "composer history stays dormant before editing");
    if (owned.text) {
        const auto words = utf8_literal("alpha  \xF0\x9F\x98\x80\xE4\xB8\x96\xE7\x95\x8C beta");
        bool word_edits = message_composer_restore(&owned, 7, words, words.bytes, words.bytes) &&
            message_composer_delete_word(&owned, false, 1) == MessageComposerResult::changed &&
            equal(owned, utf8_literal("alpha  \xF0\x9F\x98\x80\xE4\xB8\x96\xE7\x95\x8C ")) &&
            message_composer_undo(&owned) == MessageComposerResult::changed && equal(owned, words) &&
            owned.anchor == words.bytes && owned.caret == words.bytes;
        word_edits = word_edits && message_composer_set_selection(&owned, 7, 7) == MessageComposerResult::changed &&
            message_composer_delete_word(&owned, true, 2) == MessageComposerResult::changed &&
            equal(owned, utf8_literal("alpha  beta")) &&
            message_composer_undo(&owned) == MessageComposerResult::changed && equal(owned, words) &&
            owned.anchor == 7 && owned.caret == 7;
        word_edits = word_edits && message_composer_move(&owned, MessageComposerMove::next_word, true) == MessageComposerResult::changed &&
            owned.anchor == 7 && owned.caret == words.bytes - 4 &&
            message_composer_delete_word(&owned, false, 3) == MessageComposerResult::changed &&
            equal(owned, utf8_literal("alpha  beta")) && message_composer_undo(&owned) == MessageComposerResult::changed &&
            owned.anchor == 7 && owned.caret == words.bytes - 4;
        check(word_edits, "composer_word_delete_and_navigation_keep_unicode_and_single_command_undo");
        check(message_composer_restore(&owned, 7, utf8_literal("one... two"), 7, 7) &&
            message_composer_delete_word(&owned, false, 4) == MessageComposerResult::changed &&
            equal(owned, utf8_literal("onetwo")) &&
            message_composer_move(&owned, MessageComposerMove::document_home, false) == MessageComposerResult::changed &&
            message_composer_delete_word(&owned, false, 5) == MessageComposerResult::unchanged,
            "composer_word_delete_whitespace_punctuation_and_document_boundary");
        message_composer_reset(&owned, 7, false);
        message_composer_insert_scalar(&owned, 'a', 1);
        message_composer_insert_utf16(&owned, 0xD83D, 2); message_composer_insert_utf16(&owned, 0xDE00, 3);
        check(owned.history && message_composer_undo(&owned) == MessageComposerResult::changed && !owned.bytes &&
            message_composer_redo(&owned) == MessageComposerResult::changed && equal(owned, utf8_literal("a\xF0\x9F\x98\x80")),
            "composer lazily records a Unicode typing run through shared history");
        message_composer_set_selection(&owned, 5, 1);
        message_composer_insert_utf8(&owned, utf8_literal("paste"));
        check(message_composer_undo(&owned) == MessageComposerResult::changed && owned.anchor == 5 && owned.caret == 1,
            "composer history restores reversed paste selection");
        message_composer_set_selection(&owned, owned.bytes, owned.bytes);
        message_composer_replace_range(&owned, 0, 1, utf8_literal("<@123>"));
        check(message_composer_undo(&owned) == MessageComposerResult::changed && owned.anchor == 5 && owned.caret == 5 &&
            equal(owned, utf8_literal("a\xF0\x9F\x98\x80")), "composer autocomplete replacement restores pre-command caret");
        message_composer_backspace(&owned, 10); message_composer_backspace(&owned, 11);
        check(!owned.bytes && message_composer_undo(&owned) == MessageComposerResult::changed && owned.anchor == 5 && owned.caret == 5,
            "composer repeated deletion undoes as one run");
        message_composer_insert_utf16(&owned, 0xD83D, 12);
        message_composer_reset(&owned, 8, true);
        check(message_composer_insert_utf16(&owned, 0xDE00, 13) == MessageComposerResult::invalid_input,
            "composer channel reset discards incomplete UTF16 input");
        check(message_composer_undo(&owned) == MessageComposerResult::unchanged && owned.channel_id == 8 && owned.bytes == 5,
            "composer retained text cannot carry undo across channels");
        message_composer_insert_utf8(&owned, utf8_literal("x"));
        check(!message_composer_restore(&owned, 0, utf8_literal("invalid"), 8, 8) &&
            message_composer_undo(&owned) == MessageComposerResult::changed, "composer invalid restoration preserves undo");
        check(message_composer_restore(&owned, 8, utf8_literal("edit target"), 2, 4) &&
            message_composer_undo(&owned) == MessageComposerResult::unchanged && owned.anchor == 2 && owned.caret == 4,
            "composer document restoration separates edit-mode history");
    }
    message_composer_release(&owned);
    check(!owned.text && !owned.history, "composer history releases with its text owner");

    MessageRecord records[4]{}; u32 order[4]{}, heights[4]{}; u64 fenwick[4]{};
    u8 text[512]{}, draft_text[512]{}, request_text[kMessageComposerMaximumBytes]{}; char input[128]{};
    MessageDraftRecord draft_records[4]{};
    MessageStore store{}; UiVariableList list{}; MessageTimeline timeline{}; MessageDraftStore drafts{};
    MessageComposer composer{}; MessageComposerController controller{};
    const bool initialized = message_store_initialize(&store, {records, 4}, {order, 4}, {text, sizeof(text)}) &&
        ui_variable_list_initialize(&list, {heights, 4}, {fenwick, 4}, 0, 100) &&
        message_timeline_initialize(&timeline, &store, &list, 100, 100, true) &&
        message_draft_store_initialize(&drafts, {draft_records, 4}, {draft_text, sizeof(draft_text)}) &&
        message_composer_initialize(&composer, {input, sizeof(input)}) && message_composer_enable_history(&composer, 1024) &&
        message_composer_controller_initialize(&controller, &composer, &drafts, &timeline, {request_text, sizeof(request_text)});
    check(initialized, "composer history controller fixture initializes");
    if (initialized) {
        message_composer_controller_select_channel(&controller, 7);
        message_composer_insert_utf8(&composer, utf8_literal("first draft"));
        check(message_composer_controller_select_channel(&controller, 8).ok(), "composer navigation saves first draft");
        message_composer_insert_utf8(&composer, utf8_literal("second draft"));
        check(message_composer_controller_select_channel(&controller, 7).ok() && equal(composer, utf8_literal("first draft")) &&
            message_composer_undo(&composer) == MessageComposerResult::unchanged, "composer channel restore keeps text without foreign undo");
        message_composer_clear(&composer);
        for (u32 i = 0; i < 5; ++i) message_composer_insert_scalar(&composer, "hello"[i], 1 + i);
        check(!message_composer_controller_queue_send(&controller, false, 9, 1725012296789ull, 10).ok() &&
            message_composer_undo(&composer) == MessageComposerResult::changed && !composer.bytes &&
            message_composer_redo(&composer) == MessageComposerResult::changed, "composer rejected send preserves editable history");
        const auto queued = message_composer_controller_queue_send(&controller, true, 9, 1725012296789ull, 11);
        check(queued.ok() && !composer.bytes && message_composer_undo(&composer) == MessageComposerResult::unchanged &&
            message_composer_redo(&composer) == MessageComposerResult::unchanged, "composer queued send clears both history directions");
        for (u32 i = 0; i < 4; ++i) message_composer_insert_scalar(&composer, "next"[i], 20 + i);
        check(message_composer_undo(&composer) == MessageComposerResult::changed && !composer.bytes &&
            message_composer_controller_commit_draft(&controller).ok(), "composer undo updates only current draft");
        MessageComposerSubmit submit{};
        check(message_composer_controller_prepare_submit(&controller, 30, &submit).ok() &&
            submit.content.bytes == 5 && bytes_equal(submit.content.data, "hello", 5), "composer draft undo leaves queued request payload intact");
    }
    message_composer_release(&composer);
}
}
