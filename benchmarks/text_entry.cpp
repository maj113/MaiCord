#include "maicord/core/time.hpp"
#include "maicord/ui/secure_edit.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/ui/text_edit_menu.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/text_presentation.hpp"

#include <Windows.h>

namespace maicord {

bool benchmark_text_entry(Clock clock, void (*write)(const char*, u64, const char*) noexcept) noexcept {
    constexpr u32 iterations = 10000;
    char initial[1023]{}, display[4096]{};
    for (u32 i = 0; i < sizeof(initial); ++i) initial[i] = i % 32 == 31 ? '\n' : 'a';
    const u16 replacement[] = {0xD83D, 0xDE00};
    write("ui_text_entry_iterations", iterations, "cycles");
    write("ui_secure_edit_owner_size", sizeof(SecureEdit), "bytes");
    for (u32 bounded = 0; bounded < 2; ++bounded) {
        SecureEdit edit{};
        if (!secure_edit_initialize(&edit, sizeof(display), false, {bounded ? 1024u : 0u, true})) return false;
        MEMORY_BASIC_INFORMATION memory{};
        const bool queried = VirtualQuery(edit.storage.data, &memory, sizeof(memory)) == sizeof(memory);
        u64 checksum = 0;
        bool valid = queried;
        const i64 start = clock_now();
        for (u32 i = 0; valid && i < iterations; ++i) {
            SecureEditUtf16Change change{};
            SecureEditUtf16Selection selection{};
            u32 bytes = 0;
            SecureEditRenderWindow window{};
            valid = secure_edit_assign(&edit, {initial, sizeof(initial)}) == SecureEditResult::changed &&
                secure_edit_insert_scalar(&edit, '\n') == SecureEditResult::changed &&
                secure_edit_replace_utf16(&edit, 0, 1, {replacement, 2}, &change) == SecureEditResult::changed &&
                change.new_end == 2 &&
                secure_edit_utf16_selection(edit, &selection) == SecureEditResult::unchanged &&
                selection.caret == 2 &&
                secure_edit_render_window(edit, static_cast<u32>(sizeof(display)), {display, sizeof(display)}, &bytes,
                                          &window) ==
                    SecureEditResult::unchanged &&
                bytes == 1027 && static_cast<u8>(display[0]) == 0xF0;
            checksum += bytes + selection.caret + edit.revision;
        }
        const u64 elapsed = clock_microseconds(clock, start, clock_now());
        if (bounded) valid = valid && secure_edit_insert_scalar(&edit, 'X') == SecureEditResult::capacity;
        write(bounded ? "ui_text_entry_bounded_us" : "ui_text_entry_byte_bound_us", elapsed, "us");
        write(bounded ? "ui_text_entry_bounded_checksum" : "ui_text_entry_byte_bound_checksum", checksum, "sum");
        write(bounded ? "ui_text_entry_bounded_committed" : "ui_text_entry_byte_bound_committed",
            memory.RegionSize, "bytes");
        secure_edit_release(&edit);
        if (!valid) return false;
    }
    // Equivalent single-scalar native replacements with and without history.
    // Setup/teardown is outside timing; the ring reaches its steady 64-step cap.
    constexpr u32 replacement_iterations = 100000, replay_iterations = 1000000;
    write("ui_text_entry_history_replace_iterations", replacement_iterations, "replacements");
    write("ui_text_entry_history_replay_iterations", replay_iterations, "cycles");
    for (u32 history = 0; history < 2; ++history) {
        SecureEdit edit{};
        if (!secure_edit_initialize(&edit, sizeof(display), false, {1024, true})) return false;
        bool valid = secure_edit_assign(&edit, {initial, sizeof(initial)}) == SecureEditResult::changed &&
            (!history || secure_edit_enable_history(&edit, 32 * 1024));
        u64 checksum = 0;
        const i64 start = clock_now();
        for (u32 i = 0; valid && i < replacement_iterations; ++i) {
            const u16 scalar = i % 2 ? 'a' : 'b';
            SecureEditUtf16Change change{};
            valid = secure_edit_replace_utf16(&edit, 0, 1, {&scalar, 1}, &change) == SecureEditResult::changed;
            checksum += edit.storage.bytes + edit.storage.data[0];
        }
        write(history ? "ui_text_entry_history_replace_us" : "ui_text_entry_no_history_replace_us",
            clock_microseconds(clock, start, clock_now()), "us");
        write(history ? "ui_text_entry_history_replace_checksum" : "ui_text_entry_no_history_replace_checksum", checksum, "sum");
        if (history) {
            const auto costs = secure_edit_history_costs(edit);
            write("ui_text_entry_history_owner_bytes", costs.owner_bytes, "bytes");
            write("ui_text_entry_history_payload_bytes", costs.payload_bytes, "bytes");
            write("ui_text_entry_history_used_bytes", costs.used_bytes, "bytes");
            write("ui_text_entry_history_undo_steps", costs.undo_steps, "steps");
            checksum = 0;
            const i64 replay_start = clock_now();
            for (u32 i = 0; valid && i < replay_iterations; ++i) {
                valid = secure_edit_undo(&edit) == SecureEditResult::changed && edit.storage.data[0] == 'b' &&
                    secure_edit_redo(&edit) == SecureEditResult::changed && edit.storage.data[0] == 'a';
                checksum += edit.storage.bytes;
            }
            write("ui_text_entry_history_undo_redo_us", clock_microseconds(clock, replay_start, clock_now()), "us");
            write("ui_text_entry_history_undo_redo_checksum", checksum, "sum");
            constexpr u32 group_iterations = 10000;
            valid = valid && secure_edit_begin_history_group(&edit);
            secure_edit_end_history_group(&edit); // Materialize reusable scratch outside timing.
            checksum = 0;
            const i64 group_start = clock_now();
            for (u32 i = 0; valid && i < group_iterations; ++i) {
                valid = secure_edit_begin_history_group(&edit);
                SecureEditUtf16Change change{};
                for (u32 j = 0; valid && j < 10; ++j) {
                    const u16 candidate = j == 9 ? (i % 2 ? 'a' : 'b') : static_cast<u16>('c' + j);
                    valid = secure_edit_replace_utf16(&edit, 0, 1, {&candidate, 1}, &change) == SecureEditResult::changed;
                }
                secure_edit_end_history_group(&edit);
                checksum += edit.storage.bytes + edit.storage.data[0];
            }
            write("ui_text_entry_history_group_10000_us", clock_microseconds(clock, group_start, clock_now()), "us");
            write("ui_text_entry_history_group_checksum", checksum, "sum");
            const auto grouped = secure_edit_history_costs(edit);
            write("ui_text_entry_history_group_scratch_bytes", grouped.group_bytes, "bytes");
            write("ui_text_entry_history_group_retained_bytes", grouped.used_bytes, "bytes");
            valid = valid && grouped.undo_steps == 64 && grouped.used_bytes == 128 &&
                secure_edit_undo(&edit) == SecureEditResult::changed && edit.storage.data[0] == 'b' &&
                secure_edit_redo(&edit) == SecureEditResult::changed && edit.storage.data[0] == 'a';
        }
        secure_edit_release(&edit);
        if (!valid) return false;
    }
    SecureEdit typing{};
    if (!secure_edit_initialize(&typing, 4096, false, {1024, true}) || !secure_edit_enable_history(&typing, 32 * 1024)) {
        secure_edit_release(&typing); return false;
    }
    secure_edit_insert_scalar(&typing, 'x', 1); secure_edit_undo(&typing); // Warm reusable snapshot.
    u64 checksum = 0;
    bool valid = true;
    const i64 typing_start = clock_now();
    for (u32 i = 0; valid && i < 10000; ++i) {
        secure_edit_clear(&typing);
        for (u32 j = 0; valid && j < 32; ++j)
            valid = secure_edit_insert_scalar(&typing, 'a' + j % 26, static_cast<u64>(i) * 32 + j + 1) == SecureEditResult::changed;
        valid = valid && secure_edit_undo(&typing) == SecureEditResult::changed && typing.storage.bytes == 0 &&
            secure_edit_redo(&typing) == SecureEditResult::changed && typing.storage.bytes == 32;
        checksum += typing.storage.bytes;
    }
    write("ui_text_entry_typing_runs_10000_us", clock_microseconds(clock, typing_start, clock_now()), "us");
    write("ui_text_entry_typing_runs_checksum", checksum, "sum");
    const auto typing_costs = secure_edit_history_costs(typing);
    write("ui_text_entry_typing_run_records", typing_costs.undo_steps, "steps");
    write("ui_text_entry_typing_run_payload_used", typing_costs.used_bytes, "bytes");
    valid = valid && typing_costs.undo_steps == 1 && typing_costs.used_bytes == 32;
    secure_edit_release(&typing);
    MessageComposer composer{};
    if (!message_composer_initialize_owned(&composer)) return false;
    message_composer_insert_scalar(&composer, 'x', 1); message_composer_undo(&composer);
    checksum = 0;
    const i64 composer_start = clock_now();
    for (u32 i = 0; valid && i < 10000; ++i) {
        message_composer_clear(&composer);
        for (u32 j = 0; valid && j < 32; ++j)
            valid = message_composer_insert_scalar(&composer, 'a' + j % 26, static_cast<u64>(i) * 32 + j + 1) == MessageComposerResult::changed;
        valid = valid && message_composer_undo(&composer) == MessageComposerResult::changed && composer.bytes == 0 &&
            message_composer_redo(&composer) == MessageComposerResult::changed && composer.bytes == 32;
        checksum += composer.bytes;
    }
    write("ui_composer_history_10000_us", clock_microseconds(clock, composer_start, clock_now()), "us");
    write("ui_composer_history_checksum", checksum, "sum");
    const auto composer_costs = message_composer_history_costs(composer);
    write("ui_composer_owner_bytes", sizeof(MessageComposer), "bytes");
    write("ui_composer_history_owner_bytes", composer_costs.owner_bytes, "bytes");
    write("ui_composer_history_payload_bytes", composer_costs.payload_bytes, "bytes");
    write("ui_composer_history_snapshot_bytes", composer_costs.group_bytes, "bytes");
    write("ui_composer_history_used_bytes", composer_costs.used_bytes, "bytes");
    valid = valid && composer_costs.undo_steps == 1 && composer_costs.used_bytes == 32;
    message_composer_reset(&composer, 7, false);
    const u16 provisional[] = {'k'}, converted[] = {0x304B};
    TextInputChange change{};
    checksum = 0;
    const i64 native_start = clock_now();
    for (u32 i = 0; valid && i < 10000; ++i) {
        message_composer_clear(&composer);
        const auto target = message_composer_input_target(&composer);
        valid = text_input_begin_group(target) &&
            text_input_replace(target, 0, 0, {provisional, 1}, &change) == TextInputResult::changed &&
            text_input_replace(target, 0, 1, {converted, 1}, &change) == TextInputResult::changed;
        text_input_end_group(target);
        valid = valid && message_composer_undo(&composer) == MessageComposerResult::changed && !composer.bytes &&
            message_composer_redo(&composer) == MessageComposerResult::changed && composer.bytes == 3;
        checksum += composer.bytes;
    }
    write("ui_composer_native_target_10000_us", clock_microseconds(clock, native_start, clock_now()), "us");
    write("ui_composer_native_target_checksum", checksum, "sum");
    message_composer_release(&composer);
    PopupScene menu{};
    UiWidget widgets[16]{}; UiLayoutNode nodes[16]{};
    if (!popup_scene_initialize_owned(&menu, 7, {widgets, 16}, {nodes, 16})) return false;
    PopupItem items[7]{}; UiTextCommand commands[14]{}; UiTextCommandStream stream{};
    ui_text_command_stream_initialize(&stream, {commands, 14});
    UiTextOutput output{&stream, nullptr};
    const i64 menu_start = clock_now();
    checksum = 0;
    for (u32 i = 0; valid && i < 10000; ++i) {
        ui_text_command_stream_reset(&stream);
        text_edit_menu_build({false, false, (i & 1) != 0, true, true, true, true, false}, {items, 7});
        valid = popup_scene_open(&menu, PopupKind::context_menu, 7, {items, 7}, {790, 590, 791, 591}, {0, 0, 800, 600}, 260).ok() &&
            popup_text_presentation_build_output(menu, kDefaultTheme, &output) && stream.count == 14;
        checksum += popup_scene_selected_action(menu);
        popup_scene_close(&menu);
    }
    write("ui_edit_menu_open_project_close_10000_us", clock_microseconds(clock, menu_start, clock_now()), "us");
    write("ui_edit_menu_checksum", checksum, "sum");
    write("ui_edit_menu_popup_item_bytes", sizeof(PopupItem), "bytes");
    write("ui_edit_menu_widgets_and_nodes_bytes", sizeof(widgets) + sizeof(nodes), "bytes");
    write("ui_edit_menu_committed_bytes", menu.owned_storage.committed, "bytes");
    popup_scene_release(&menu);
    valid = valid && checksum == 40000;
    return valid;
}

} // namespace maicord
