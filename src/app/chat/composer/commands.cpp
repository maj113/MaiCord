#include "commands_internal.hpp"
#include "commands.hpp"
#include "maicord/app/gateway/client_event.hpp"
#include "../../shell/scrolling.hpp"
#include "../../account/account_presentation.hpp"
#include "../messages/send.hpp"
#include "text_form.hpp"
#include "attachments.hpp"
#include "../../media/picker.hpp"
#include "../../account/user_actions.hpp"
#include "maicord/protocol/messages/interaction.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"
#include "../../account/account_access.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/platform/window/key.hpp"

namespace maicord::app_detail {

bool application_commands_sync(bool* handled) noexcept {
    *handled = false;
    const auto input = message_composer_text(g_app.message_composer);
    if (!g_app.account_ui || !input.bytes || input.data[0] != '/' || g_app.message_edit.editor_active) {
        close_popup();
        if (commands) {
            reset_value(&commands->selected);
            commands->revision = 0;
        }
        // A message keeps no option end markers.
        const auto stripped = g_app.message_edit.editor_active ? MessageComposerResult::unchanged
                                                               : normalize_markers(input, false);
        return stripped == MessageComposerResult::changed ? composer_apply_result(stripped, false) : true;
    }
    *handled = true;
    if (!ensure_commands())
        return false;
    // The picker wants the account's command frecency (failure only loses the section).
    ensure_frecency();
    auto& c = *commands;
    const auto& composer = g_app.message_composer;
    if (c.dismissed_document == composer.document_identity && c.dismissed_revision == composer.text_revision) {
        close_popup();
        // The built-in's inline results are its autocomplete: dismissed with it.
        return builtin_command_results(nullptr, {}, {});
    }
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                           g_app.message_composer.channel_id);
    const u64 guild = channel ? channel->guild_id : 0;
    if (c.channel != g_app.message_composer.channel_id) {
        advance_channel_index_epoch(c);
        if (c.guild != guild || !guild) {
            c.loaded[0] = false;
            c.pending |= 1;
            command_index_release(&c.index[0]);
            c.attempts[0] = 0;
        }
        c.channel = g_app.message_composer.channel_id;
        c.guild = guild;
        reset_value(&c.selected);
        c.filter_app = 0;
        c.revision = 0;
    }
    if (!c.loaded[0])
        c.pending |= 1;
    if (!c.loaded[1])
        c.pending |= 2;
    if (c.selected.command) {
        const auto name = c.selected.command->name;
        if (input.bytes < name.bytes + 1 || !utf8_equal({input.data + 1, name.bytes}, name) ||
            (input.bytes > name.bytes + 1 && !ascii_is_space(input.data[name.bytes + 1])))
            reset_value(&c.selected);
    }
    resolve_wanted(input);
    if (!c.selected.command) {
        c.prepared = nullptr;
        reset_value(&c.focused_option);
        c.focused_slot = no_option;
        select_typed(input);
    }
    // A newly selected command gets its required options as chips; the edit
    // comes back here through the composer refresh.
    if (c.selected.command && c.prepared != c.selected.command) {
        // The composer projects the newly selected command and its chips again.
        const auto prepared = set_up_command();
        return prepared == MessageComposerResult::changed ? composer_apply_result(prepared, false)
                                                          : refresh_message_composer_ui();
    }
    const bool changed = c.revision != g_app.message_composer.revision;
    if (changed) {
        const auto normalized = normalize_markers(input, c.selected.command != nullptr);
        if (normalized == MessageComposerResult::changed)
            return composer_apply_result(normalized, false);
    }
    if (changed && c.selected.command) {
        // The chip the caret is in is the active option (normally settled by
        // the composer projection already); a command whose only option is
        // optional turns typed text into that option.
        update_focus(input);
        CommandArgumentSpan spans[25];
        if (!typed_options(input, {spans, 25})) {
            const auto single = chip_single_option(input, 0);
            if (single == MessageComposerResult::changed)
                return composer_apply_result(single, false);
        }
    }
    if (changed) {
        c.revision = g_app.message_composer.revision;
        ++c.generation;
        // A choice list focuses its first row; the unset options list (never filtered
        // here) starts without.
        c.keyboard_row = !c.selected.command ? 0
                         : c.focused_option.source.bytes && option_type(*c.selected.index, c.focused_option) != 11
                             ? 1
                             : no_row;
        reset_value(&c.error);
        if (c.focused_option.source.bytes)
            c.offset = 0;
        if (!c.selected.command && !project({input.data + 1, input.bytes - 1}))
            return false;
    }
    const auto* surface = ui_widget_manager_layout_node(g_app.message_composer_scene.widgets,
                                                         g_app.message_composer_scene.surface);
    if (autocomplete_enabled()) {
        const u64 fingerprint =
            hash64_bytes(kHash64Seed, input.data, input.bytes) ^ c.focused_option.source_offset;
        if (command_autocomplete_observe(&c.autocomplete, {c.channel, c.selected.command->id, fingerprint},
                                         clock_monotonic_milliseconds()))
            ++c.generation;
    } else
        command_autocomplete_cancel(&c.autocomplete);
    if (!changed && c.rendered_generation == c.generation && g_app.popup_scene.active &&
        g_app.popup_owner_kind == PopupOwnerKind::commands && surface &&
        ui_rect_equal(g_app.popup_scene.anchor, surface->rectangle) &&
        ui_rect_equal(g_app.popup_scene.viewport, application_viewport_dips()))
        return true;
    return render();
}
bool application_commands_activate(u32 action) noexcept {
    if (!commands)
        return true;
    auto& c = *commands;
    const u32 text_revision = g_app.message_composer.text_revision;
    MessageComposerResult edit_result = MessageComposerResult::unchanged;
    if (action == clear_selection && c.selected.command) {
        reset_value(&c.selected);
        reset_option_states(c);
        c.choice_count = c.option_count = 0;
        command_autocomplete_cancel(&c.autocomplete);
        c.offset = c.rail_offset = 0;
        c.revision = 0;
        edit_result = message_composer_clear(&g_app.message_composer);
    } else if (action >= value_base) {
        const u32 slot = action - value_base;
        const auto input = message_composer_text(g_app.message_composer);
        if (!c.selected.command || !c.focused_option.source.bytes || !c.choices_shown || slot >= c.choice_count ||
            c.value_begin > c.value_end || c.value_end > input.bytes)
            return true;
        // A suggestion's name stands in the draft; its value is what is sent.
        if (c.choice_suggested && slot < c.autocomplete.suggestions.count) {
            const auto& choice = c.autocomplete.suggestions.choices[slot];
            char storage[128];
            const auto option = field_text(*c.selected.index, c.focused_option, "name", {storage, sizeof(storage)});
            u32 at = 0;
            while (at < c.accepted_count && !utf8_equal(c.accepted[at].option, option))
                ++at;
            if (at < 25) {
                auto* text = c.accepted_text[at];
                c.accepted[at] = {copy_label(text[0], 512, option), copy_label(text[1], 512, choice.name),
                                  copy_label(text[2], 512, choice.value), choice.kind};
                c.accepted_count = maximum(c.accepted_count, at + 1);
            }
        }
        // The value is replaced by the name; the next chip's value is selected
        // then, else the caret goes to the end.
        const u32 focused = c.focused_slot;
        edit_result = message_composer_replace_range(&g_app.message_composer, c.value_begin, c.value_end,
                                                     {c.choice_names[slot], c.choice_bytes[slot]});
        CommandArgumentSpan spans[25];
        const auto now = message_composer_text(g_app.message_composer);
        const u32 count = typed_options(now, {spans, 25});
        bool next = false;
        for (u32 n = 0; n + 1 < count && !next; ++n)
            if (spans[n].slot == focused) {
                select_chip(spans[n + 1]);
                next = true;
            }
        if (!next)
            message_composer_set_selection(&g_app.message_composer, now.bytes, now.bytes);
        c.offset = 0;
    } else if (action >= option_base) {
        const auto list = command_options();
        const u32 slot = action - option_base;
        if (!c.selected.command || c.choices_shown || slot >= list.count)
            return true;
        // The ATTACHMENT chip the caret is in only asks for its file.
        if (slot == c.focused_slot)
            return application_pick_attachments() && refresh_message_composer_ui();
        // The "<name>" chip is put in place of the word at the caret:
        // a new, empty chip with the caret in it.
        char name[128], insertion[140];
        const auto key = field_text(*c.selected.index, list.options[slot], "name", {name, sizeof(name)});
        const auto input = message_composer_text(g_app.message_composer);
        const u32 caret = minimum(g_app.message_composer.caret, input.bytes);
        const u32 first = caret - caret_word(input, caret).bytes;
        const bool before = first && !ascii_is_space(input.data[first - 1]) &&
                            !(first >= kCommandOptionEndBytes && option_end_at(input, first - kCommandOptionEndBytes));
        const auto value = copy_label(insertion, sizeof(insertion), before ? utf8_literal(" ") : Utf8View{}, key,
                                      utf8_literal(":"), Utf8View{kCommandOptionEnd, kCommandOptionEndBytes});
        edit_result = message_composer_replace_range(&g_app.message_composer, first, caret, value);
        const u32 inside = first + (before ? 1u : 0u) + key.bytes + 1;
        message_composer_set_selection(&g_app.message_composer, inside, inside);
        c.offset = 0;
        // An ATTACHMENT option asks for its file.
        if (option_type(*c.selected.index, list.options[slot]) == 11 && !application_pick_attachments())
            return false;
    } else if (action >= rail_base) {
        // The rail: a section shows alone; clicking it again or the
        // clock lists everything again from the top.
        const u32 n = action - rail_base;
        if (c.selected.command || n >= c.rail_count)
            return true;
        const auto& section = c.rail[n];
        const u64 id = section.app ? section.app->id : 0;
        c.filter_app = section.kind == SectionKind::frequent || id == c.filter_app ? 0 : id;
        c.revision = 0;
    } else if (action >= choose_base && action - choose_base < c.count) {
        c.selected = c.entries[action - choose_base];
        c.revision = 0;
        c.offset = 0;
        char insertion[512];
        const auto value =
            copy_label(insertion, 512, utf8_literal("/"), c.selected.command->name, utf8_literal(" "));
        const auto input = message_composer_text(g_app.message_composer);
        const u32 prefix = c.selected.command->name.bytes + 1;
        const bool named_arguments = input.bytes > prefix && input.data[0] == '/' &&
            utf8_equal({input.data + 1, prefix - 1}, c.selected.command->name) &&
            ascii_is_space(input.data[prefix]);
        edit_result = message_composer_replace_range(&g_app.message_composer, 0,
            named_arguments ? prefix + 1 : input.bytes, value);
    }
    return edit_result == MessageComposerResult::changed
               ? composer_apply_result(edit_result, text_revision != g_app.message_composer.text_revision)
               : refresh_message_composer_ui();
}
bool application_commands_take_tts() noexcept {
    if (!commands || !commands->builtin_tts)
        return false;
    commands->builtin_tts = false;
    return commands->builtin_text_revision == g_app.message_composer.text_revision;
}
bool application_commands_send(bool* handled) noexcept {
    const auto input = message_composer_text(g_app.message_composer);
    // A built-in text command's message, rewritten in place, sends as typed.
    if (commands && commands->builtin_text) {
        commands->builtin_text = false;
        if (commands->builtin_text_revision == g_app.message_composer.text_revision) {
            *handled = false;
            return true;
        }
    }
    *handled = input.bytes && input.data[0] == '/';
    if (!*handled)
        return true;
    if (g_app.account.status.required_action != AccountRequirement::none) return true;
    if (!commands || !commands->selected.command) {
        bool ignored;
        return application_commands_sync(&ignored);
    }
    auto& c = *commands;
    bool blocked = false;
    if (!send_check(&blocked))
        return false;
    if (blocked)
        return true;
    if (local_command_application(c.selected.command->application_id)) {
        char options[16384];
        ProtocolWriter writer{{reinterpret_cast<u8*>(options), sizeof(options)}, 0};
        const u32 start = c.selected.command->name.bytes + 1;
        if (input.bytes < start || !command_arguments(*c.selected.index, *c.selected.command,
                                                      {input.data + start, input.bytes - start}, &writer, &c.error,
                                                      {}, {}, {}, nullptr, accepted_choices()))
            return render();
        // The commands popup goes first: what a built-in opens (a confirmation,
        // the DM) must not find it in front. Closing it is no dismissal: a
        // built-in that does not run shows it again.
        const u64 dismissed_document = c.dismissed_document;
        const u32 dismissed_revision = c.dismissed_revision;
        close_popup();
        const auto reopen = [&]() noexcept {
            c.dismissed_document = dismissed_document;
            c.dismissed_revision = dismissed_revision;
            return render();
        };
        char text_storage[kMessageComposerMaximumBytes];
        Utf8View text{};
        const auto run = plugin_command_application(c.selected.command->application_id)
                             ? plugin_command_execute(*c.selected.command, {options, writer.used}, c.channel, c.guild,
                                                      {text_storage, sizeof(text_storage)}, &text)
                             : builtin_command_run(*c.selected.command, {options, writer.used}, c.channel,
                                                   {text_storage, sizeof(text_storage)}, &text);
        if (run == BuiltinCommandRun::failed) {
            c.error = utf8_literal("This command couldn't run. Try again.");
            return reopen();
        }
        // /gif and /sticker send only a result: their results took the Enter
        // (the command ended once one sent; else it waits for loading results)...
        if (run == BuiltinCommandRun::results)
            return c.selected.command ? reopen() : true;
        // ...and with none to send the draft stays, the app shakes and the bar says why.
        if (run == BuiltinCommandRun::rejected) {
            c.error = application_media_picker_inline_message();
            shake_app();
            return reopen() && refresh_message_composer_ui();
        }
        // A built-in's use is recorded as it runs (a plugin's is not: its ids are not Discord's).
        frecency_track(*c.selected.command, c.guild);
        const bool speaks = run == BuiltinCommandRun::text && !plugin_command_application(c.selected.command->application_id) &&
                            builtin_command_speaks(*c.selected.command);
        builtin_command_results(nullptr, {}, {});
        command_autocomplete_cancel(&c.autocomplete);
        reset_value(&c.error);
        reset_value(&c.selected);
        close_popup();
        if (run == BuiltinCommandRun::handled)
            return composer_apply_result(message_composer_clear(&g_app.message_composer), true);
        // A text command's result is sent as the message itself; an insert stays in the draft.
        const auto replaced = message_composer_replace_range(&g_app.message_composer, 0,
                                                             g_app.message_composer.bytes, text);
        if (replaced == MessageComposerResult::invalid_input)
            return true;
        if (run == BuiltinCommandRun::text) {
            c.builtin_text = true;
            c.builtin_tts = speaks;
            c.builtin_text_revision = g_app.message_composer.text_revision;
            *handled = false;
        }
        return composer_apply_result(replaced, true);
    }
    if (c.sending || c.queued)
        return true;
    // ATTACHMENT options take the channel's staged uploads (staged through the
    // channel attachments endpoint).
    UploadedAttachment uploads[kAttachmentUploadMaximumCount];
    u64 upload_ids[kAttachmentUploadMaximumCount];
    Utf8View files[kAttachmentUploadMaximumCount];
    u32 upload_count = 0;
    if (has_attachment_option(*c.selected.index, *c.selected.command)) {
        if (!application_attachment_prepare_send(c.channel, {uploads, kAttachmentUploadMaximumCount},
                                                 {upload_ids, kAttachmentUploadMaximumCount}, &upload_count)) {
            c.error = utf8_literal("Wait for the file to finish uploading");
            return render();
        }
        for (u32 n = 0; n < upload_count; ++n)
            files[n] = uploads[n].filename;
    }
    char options[16384];
    ProtocolWriter writer{{reinterpret_cast<u8*>(options), sizeof(options)}, 0};
    const u32 start = c.selected.command->name.bytes + 1;
    CommandAttachmentPlan plan{};
    if (input.bytes < start ||
        !command_arguments(*c.selected.index, *c.selected.command,
                           {input.data + start, input.bytes - start}, &writer, &c.error, {}, {},
                           {files, upload_count}, &plan, accepted_choices()))
        return render();
    UploadedAttachment staged[kAttachmentUploadMaximumCount];
    u64 staged_ids[kAttachmentUploadMaximumCount];
    for (u32 k = 0; k < plan.count; ++k) {
        staged[k] = uploads[plan.file[k]];
        staged[k].id = k;
        staged_ids[k] = upload_ids[plan.file[k]];
    }
    u8 attachments[8192];
    const u32 attachment_bytes =
        plan.count ? attachment_upload_write_references({staged, plan.count}, {attachments, sizeof(attachments)}) : 0;
    ProtocolWriter data{{reinterpret_cast<u8*>(c.payload), sizeof(c.payload)}, 0};
    if ((plan.count && !attachment_bytes) ||
        !command_data(*c.selected.command, {options, writer.used}, &data,
                      {reinterpret_cast<const char*>(attachments), attachment_bytes})) {
        c.error = utf8_literal("Command is too large to send");
        return render();
    }
    c.payload_bytes = data.used;
    frecency_track(*c.selected.command, c.guild);
    queue_command(c.selected.command->application_id,
                  echo_author(c.selected.command->application_id, c.selected.app), c.selected.command->name,
                  message_type_chat_command);
    if (plan.count) {
        // The uploads now belong to this command; a command with files offers no resend.
        application_attachment_did_queue({staged_ids, plan.count}, c.nonce);
        if (Echo* echo = find_echo(c.nonce, 0))
            echo->data_bytes = 0;
    }
    command_autocomplete_cancel(&c.autocomplete);
    reset_value(&c.error);
    reset_value(&c.selected);
    close_popup();
    // The draft clears as the command is sent; its
    // outcome is the local command message.
    return composer_apply_result(message_composer_clear(&g_app.message_composer), true);
}
bool application_commands_pending(u64 now) noexcept {
    if (!commands || !g_app.account_ui || !g_app.account.api.application_command) return false;
    const auto& c = *commands;
    const bool suggest = c.autocomplete.phase == CommandAutocompletePhase::debounce && now >= c.autocomplete.due_at;
    // The picker, or a selected command showing only its option bar, wants the indexes.
    const bool lane = c.queued || builtin_commands_pending() ||
                      ((c.pending || suggest) && (c.context_wanted || g_app.popup_owner_kind == PopupOwnerKind::commands ||
                                                  command_in_draft()));
    return !c.sending && c.retry_at <= now && (lane || frecency_fetch_due(now) || frecency_save_due(now));
}
bool application_commands_submit(u64 now) noexcept {
    if (!application_commands_pending(now)) return false;
    auto& c = *commands;
    // A built-in /nick, /thread, /kick, /ban or /timeout, after a queued interaction.
    if (!c.queued && builtin_commands_pending()) {
        const u64 request_id = application_next_request_id();
        bool busy = false;
        if (builtin_commands_submit(request_id, &busy)) {
            c.operation = builtin_operation;
            c.retry_at = 0;
            g_app.details_request_owner = MessageHistoryRequestOwner::commands;
            g_app.details_request_id = request_id;
        } else if (busy)
            c.retry_at = now + 100;
        return true;
    }
    const u8 slot = (c.pending & 1) ? 0 : 1;
    const bool popup = g_app.popup_owner_kind == PopupOwnerKind::commands;
    const bool indexes = c.pending && (c.context_wanted || popup || command_in_draft());
    const bool suggest = popup && c.autocomplete.phase == CommandAutocompletePhase::debounce &&
                         now >= c.autocomplete.due_at;
    const bool fetch = frecency_fetch_due(now), save = !fetch && frecency_save_due(now);
    if (!c.queued && !indexes && !suggest && !fetch && !save)
        return false;
    const u8 op = c.queued ? 2 : indexes ? slot : suggest ? 3 : fetch ? 4 : 5;
    u64 nonce = op == 2 ? c.nonce : op == 3 ? next_nonce() : 0;
    u64 application = op == 2 ? c.send_application : 0;
    Utf8View payload = op == 2 ? Utf8View{c.payload, c.payload_bytes} : Utf8View{};
    if (op == 5) {
        auto& f = *frecency;
        const u32 bytes = command_frecency_write_patch({f.entries, f.count}, f.data_version,
                                                       {f.scratch, sizeof(f.scratch)}, {f.json, sizeof(f.json)});
        if (!bytes) {
            log_write(LogLevel::warning, "commands", "frecency_patch_capacity");
            f.save_due = 0;
            return true;
        }
        payload = {reinterpret_cast<const char*>(f.json), bytes};
    }
    if (op == 3) {
        char name_storage[128], options[16384];
        const auto name = field_text(*c.selected.index, c.focused_option, "name", {name_storage, 128});
        const auto input = message_composer_text(g_app.message_composer);
        const u32 start = c.selected.command->name.bytes + 1;
        ProtocolWriter w{{reinterpret_cast<u8*>(options), sizeof(options)}, 0};
        Utf8View error{};
        ProtocolWriter data{{reinterpret_cast<u8*>(c.payload), sizeof(c.payload)}, 0};
        if (start > input.bytes || !c.focused_option.source.bytes ||
            !command_arguments(*c.selected.index, *c.selected.command,
                               {input.data + start, input.bytes - start}, &w, &error, name, focused_query(), {},
                               nullptr, accepted_choices()) ||
            !command_data(*c.selected.command, {options, w.used}, &data)) {
            command_autocomplete_begin(&c.autocomplete, nonce, now);
            command_autocomplete_transport(&c.autocomplete, nonce, false);
            c.suggestions_dirty = true;
            return true;
        }
        application = c.selected.command->application_id;
        payload = {c.payload, data.used};
    }
    const u64 request_id = application_next_request_id();
    const auto result = g_app.account.api.application_command(
        g_app.account.instance, op == 2 ? c.send_channel : c.channel, op == 2 ? c.send_guild : c.guild,
        application, nonce, payload, request_id, op);
    if (result.error == RestClientSubmitError::busy) {
        c.retry_at = now + 100;
        c.indexing_retry = false;
        return true;
    }
    if (op == 3)
        command_autocomplete_begin(&c.autocomplete, nonce, now);
    if (!result.ok()) {
        if (op == 3) {
            command_autocomplete_transport(&c.autocomplete, nonce, false);
            c.suggestions_dirty = true;
        } else if (op >= 4) {
            // Refused locally: try again with the next picker after a pause.
            log_u64(LogLevel::warning, "commands", "frecency_refused", static_cast<u64>(result.error));
            frecency->retry_after = now + frecency_retry_ms;
        } else if (op == 2) {
            c.queued = false;
            application_attachment_complete(nonce);
            if (Echo* echo = find_echo(nonce, 0))
                echo_fail(*echo, {});
        } else {
            c.error = utf8_literal("Couldn't load commands. Try again.");
            c.dismissed_document = 0;
            c.revision = g_app.message_composer.revision;
            c.pending = 0;
            c.loaded[0] = c.loaded[1] = true;
            ++c.generation;
            refresh_message_composer_ui();
        }
        return true;
    }
    c.operation = op;
    c.requested_slot = slot;
    c.issued_nonce = nonce;
    c.requested_channel = op == 2 ? c.send_channel : c.channel;
    c.requested_guild = op == 2 ? c.send_guild : c.guild;
    c.requested_index_epoch = c.channel_index_epoch;
    c.retry_at = 0;
    c.indexing_retry = false;
    if (op == 2) {
        c.queued = false;
        c.sending = true;
    } else if (op < 2) {
        c.pending &= ~(1u << slot);
    } else if (op == 4) {
        frecency->fetching = true;
        frecency->revision = g_app.account.status.frecency_settings_revision;
    } else if (op == 5) {
        frecency->saving = true;
        frecency->sent_pending = frecency->pending_count;
    }
    g_app.details_request_owner = MessageHistoryRequestOwner::commands;
    g_app.details_request_id = request_id;
    return true;
}
bool application_commands_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!commands)
        return true;
    auto& c = *commands;
    if (c.catalog_dirty && !application_commands_tick(clock_monotonic_milliseconds()))
        return false;
    if (c.operation == builtin_operation) {
        c.operation = 0;
        return builtin_commands_response(response, body);
    }
    const bool same = c.requested_channel == c.channel && c.requested_guild == c.guild;
    if (c.operation == 0 && c.requested_index_epoch != c.channel_index_epoch)
        return refresh_message_composer_ui();
    if (c.operation == 3) {
        c.suggestions_dirty =
            command_autocomplete_transport(&c.autocomplete, c.issued_nonce, response.succeeded()) ||
            c.suggestions_dirty;
        return application_commands_tick(clock_monotonic_milliseconds());
    }
    if (c.operation >= 4) {
        if (!frecency)
            return true;
        auto& f = *frecency;
        const bool save = c.operation == 5;
        const u64 now = clock_monotonic_milliseconds();
        f.fetching = f.saving = false;
        const bool ok = response.succeeded() &&
                        frecency_install(body, save ? f.sent_pending : 0);
        f.sent_pending = 0;
        if (!ok) {
            log_u64(LogLevel::warning, "commands", save ? "frecency_save_status" : "frecency_fetch_status",
                    response.status_code);
            // A refused save is retried over a fresh copy, after a pause.
            if (save)
                f.loaded = false;
            f.retry_after = now + frecency_retry_ms;
        }
        f.save_due = f.loaded && f.pending_count ? maximum(f.save_due, now + frecency_save_delay_ms) : 0;
        if (!ok || g_app.popup_owner_kind != PopupOwnerKind::commands || c.selected.command)
            return true;
        const auto input = message_composer_text(g_app.message_composer);
        if (input.bytes && !project({input.data + 1, input.bytes - 1}))
            return false;
        return refresh_message_composer_ui();
    }
    if (c.operation == 2) {
        c.sending = false;
        // Uploads a command took are spent either way (no resend with files).
        application_attachment_complete(c.issued_nonce);
        if (!response.succeeded()) {
            log_u64(LogLevel::warning, "commands", "command_status", response.status_code);
            CommandInteractionFailure failure{};
            char message[512];
            if (response.transport_ok() && response.status_code >= 400 && response.status_code < 500)
                command_interaction_failure(body,
                                            {g_app.account_ui->message_history_page_structurals,
                                             kMessageHistoryPageStructuralCount},
                                            {message, sizeof(message)}, &failure);
            if (failure.stale_index)
                mark_indexes_stale(c);
            if (Echo* echo = find_echo(c.issued_nonce, 0))
                echo_fail(*echo, failure.message);
        }
    } else if (same || c.requested_slot == 1) {
        if (response.transport_ok() &&
            ((response.status_code == 202 && c.attempts[c.requested_slot]++ < 3) ||
             (response.status_code == 429 && response.rate_limited && response.retry_at_ms &&
              !api_rejection_requires_action(response.rejection)))) {
            c.pending |= 1u << c.requested_slot;
            c.retry_at = response.status_code == 429 ? response.retry_at_ms :
                deadline_after(clock_monotonic_milliseconds(), 5000);
            c.indexing_retry = c.operation == 0 && response.status_code == 202;
        } else {
            c.pending &= ~(1u << c.requested_slot);
            c.loaded[c.requested_slot] = true;
            if (response.transport_ok() && response.status_code == 200) {
                if (c.selected.index == &c.index[c.requested_slot])
                    reset_value(&c.selected);
                if (!command_index_decode(body,
                                          {g_app.account_ui->message_history_page_structurals,
                                           kMessageHistoryPageStructuralCount},
                                          &c.index[c.requested_slot]))
                    c.error = utf8_literal("Couldn't load commands");
            } else
                c.error = utf8_literal("Couldn't load commands");
            const auto input = message_composer_text(g_app.message_composer);
            if (!project(input.bytes ? Utf8View{input.data + 1, input.bytes - 1} : Utf8View{}))
                return false;
            resolve_wanted(input);
            if (c.context_wanted && !c.pending)
                c.context_wanted = false;
            ++c.catalog_revision;
            application_user_actions_commands_changed();
        }
    }
    ++c.generation;
    return refresh_message_composer_ui();
}
bool application_commands_scroll_range(bool rail, UiScrollRange* range) noexcept {
    if (!commands || g_app.popup_owner_kind != PopupOwnerKind::commands || !g_app.popup_scene.active)
        return false;
    const auto& c = *commands;
    auto rect = g_app.popup_scene.rectangle;
    if (c.selected.command) {
        if (rail || c.list_height <= 0.f)
            return false;
        *range = {rect, c.offset,
                  maximum(0.0, static_cast<double>(c.list_height + kCommandListInset - ui_rect_height(rect))),
                  kCommandGenericRowHeight, 1, 0, c.generation};
        return true;
    }
    // The picker's body runs to the panel edges after the rail card and its
    // gap; query results have no rail.
    if (rail && !c.discovery)
        return false;
    if (c.discovery)
        rect = rail ? popup_scene_picker_rail(g_app.popup_scene) : popup_scene_picker_body(g_app.popup_scene);
    const double height = rail ? rail_extent(c) : body_extent(c);
    *range = {rect,
              rail ? c.rail_offset : c.offset,
              maximum(0.0, height - ui_rect_height(rect)),
              rail ? kCommandRailStride : kCommandRowHeight,
              1,
              0,
              c.generation};
    return true;
}
bool application_commands_scroll(bool rail, double offset, bool* changed) noexcept {
    if (!commands)
        return false;
    auto& c = *commands;
    float& target = rail ? c.rail_offset : c.offset;
    *changed = target != static_cast<float>(offset);
    target = static_cast<float>(offset);
    return !*changed || render();
}

namespace {

// Brings entry k's row into view, below the pinned header of its section.
bool reveal_row(const Commands& c, u32 k) noexcept {
    const float top = row_top(c, k);
    return reveal_command_row(c.discovery ? top - kCommandSectionHeight : top, top + kCommandRowHeight);
}
bool reveal_rail(const Commands& c, u32 r) noexcept {
    const float top = rail_top(c, r);
    return reveal_command_row(top - kCommandRailLead, top + kCommandRailIcon + kCommandListInset, true);
}
// The rail keys: Up/Down/Home/End move, Enter/Space click,
// Right, Tab or Escape return to the list.
bool rail_key(Commands& c, u32 key) noexcept {
    const u32 r = c.rail_focus;
    if (key == key::up || key == key::down || key == key::home || key == key::end) {
        c.rail_focus = key == key::home ? 0
                       : key == key::end ? c.rail_count - 1
                       : key == key::up  ? (r ? r - 1 : 0)
                                       : minimum(r + 1, c.rail_count - 1);
        return reveal_rail(c, c.rail_focus) && render();
    }
    if (key == key::enter || key == key::space) {
        // The key's character must not reach the draft.
        g_app.composer_menu_held_key = key;
        if (!application_commands_activate(rail_base + r))
            return false;
        c.rail_focus = minimum(r, c.rail_count ? c.rail_count - 1 : 0);
        return reveal_rail(c, c.rail_focus) && render();
    }
    if (key == key::right || key == key::tab || key == key::escape) {
        c.rail_focus = no_row;
        // Into the list at the section the rail entry names (Escape keeps the row).
        if (key != key::escape && r < c.rail_count) {
            const auto& section = c.rail[r];
            for (u32 s = 0; s < c.body_count; ++s)
                if (c.body[s].kind == section.kind && c.body[s].app == section.app && c.body[s].count) {
                    c.keyboard_row = c.body[s].first;
                    break;
                }
        }
        return (c.keyboard_row == no_row || reveal_row(c, c.keyboard_row)) && render();
    }
    // Left stays in the rail; typing goes to the draft (which leaves the rail).
    return key == key::left;
}
bool picker_key(Commands& c, u32 key) noexcept {
    if (c.rail_focus != no_row && c.rail_focus < c.rail_count)
        return rail_key(c, key);
    if (key == key::left && c.discovery && c.rail_count) {
        c.rail_focus = minimum(active_rail(c), c.rail_count - 1);
        return reveal_rail(c, c.rail_focus) && render();
    }
    if (key == key::enter || key == key::tab) {
        if (c.count && c.keyboard_row < c.count)
            return application_commands_activate(choose_base + c.keyboard_row);
        // Without a keyboard selection Tab first selects the discovery list's
        // top row; otherwise the key keeps its composer meaning.
        if (key == key::tab && c.discovery && c.count) {
            c.keyboard_row = 0;
            return reveal_row(c, 0) && render();
        }
        return false;
    }
    if (key == key::up || key == key::down) {
        if (!c.count)
            return true;
        const u32 last = c.count - 1;
        // Discovery stops at its ends; query results wrap.
        if (c.keyboard_row > last)
            c.keyboard_row = key == key::up && !c.discovery ? last : 0;
        else if (key == key::up)
            c.keyboard_row = c.keyboard_row ? c.keyboard_row - 1 : c.discovery ? 0 : last;
        else
            c.keyboard_row = c.keyboard_row < last ? c.keyboard_row + 1 : c.discovery ? last : 0;
        return reveal_row(c, c.keyboard_row) && render();
    }
    return false;
}

} // namespace

bool application_commands_key(u32 key) noexcept {
    if (!commands)
        return true;
    auto& c = *commands;
    if (!c.selected.command)
        return picker_key(c, key);
    bool handled = false;
    if (!application_media_picker_inline_key(key, &handled))
        return false;
    if (handled)
        return true;
    if (key == key::escape)
        return false;
    const u32 count = c.option_count;
    const bool row = c.keyboard_row != no_row && c.keyboard_row >= 1 && c.keyboard_row <= count;
    if (key == key::enter || key == key::tab) {
        // A selected row is taken; else Tab moves to the next option and Enter
        // sends.
        if (const u32 action = row ? popup_scene_selected_action(g_app.popup_scene) : 0)
            return application_commands_activate(action);
        if (key == key::tab)
            return application_commands_option_tab(false);
        bool sent;
        // A built-in text command leaves its message for the ordinary send.
        return application_commands_send(&sent) && (sent || queue_message_send());
    }
    if (key == key::up || key == key::down) {
        if (!count)
            return true;
        if (!row)
            c.keyboard_row = key == key::up ? count : 1;
        else if (key == key::up)
            c.keyboard_row = c.keyboard_row > 1 ? c.keyboard_row - 1 : count;
        else
            c.keyboard_row = c.keyboard_row < count ? c.keyboard_row + 1 : 1;
        // The row's place in the list (titles, their gaps and placeholders included).
        const auto items = ConstSlice<PopupItem>{g_app.popup_scene.items.data, g_app.popup_scene.item_count};
        float top = 0.f;
        for (u32 n = 0, enabled = 0; n < items.count; ++n) {
            if (n && (items[n].flags & popup_item_command_section))
                top += kCommandSectionGap;
            if ((items[n].flags & popup_item_enabled) && ++enabled == c.keyboard_row)
                break;
            top += list_item_height(items[n]);
        }
        return reveal_command_row(top, top + kCommandGenericRowHeight) && render();
    }
    return false;
}
bool application_commands_option_tab(bool reverse) noexcept {
    if (!commands || !commands->selected.command || g_app.message_edit.editor_active)
        return false;
    const auto& c = *commands;
    const auto input = message_composer_text(g_app.message_composer);
    CommandArgumentSpan spans[25];
    const u32 count = typed_options(input, {spans, 25});
    const u32 caret = g_app.message_composer.caret;
    const u32 current = chip_at(spans, count, caret);
    u32 target = no_option;
    for (u32 n = 0; n < count; ++n)
        if (n != current && (reverse ? spans[n].key_first < caret : spans[n].key_first >= caret)) {
            target = n;
            if (!reverse)
                break;
        }
    // Past the last chip the caret goes to the end; before the first, to the
    // end of the command's name.
    const u32 name_end = c.selected.command->name.bytes + 1;
    const auto result = target != no_option ? select_chip(spans[target])
                        : !reverse          ? message_composer_set_selection(&g_app.message_composer, input.bytes,
                                                                             input.bytes)
                        : caret > name_end  ? message_composer_set_selection(&g_app.message_composer, name_end,
                                                                             name_end)
                                            : MessageComposerResult::unchanged;
    return result != MessageComposerResult::changed || composer_apply_result(result, false);
}
void application_commands_prepare_delete(bool forward) noexcept {
    auto& composer = g_app.message_composer;
    if (!command_in_draft() || composer.anchor != composer.caret)
        return;
    const auto input = message_composer_text(composer);
    CommandArgumentSpan spans[25];
    const u32 count = typed_options(input, {spans, 25});
    for (u32 n = 0; n < count; ++n) {
        if (spans[n].end_last == spans[n].value_last)
            continue;
        if (!forward && composer.caret == spans[n].end_last)
            message_composer_set_selection(&composer, spans[n].value_last, spans[n].value_last);
        else if (forward && composer.caret == spans[n].value_last)
            message_composer_set_selection(&composer, spans[n].end_last, spans[n].end_last);
        else
            continue;
        return;
    }
}
bool application_commands_close() noexcept {
    return !commands || !commands->selected.command || application_commands_activate(clear_selection);
}
bool application_commands_builtin_used(Utf8View replacement) noexcept {
    if (!commands || !commands->selected.command)
        return true;
    auto& c = *commands;
    frecency_track(*c.selected.command, c.guild);
    command_autocomplete_cancel(&c.autocomplete);
    reset_value(&c.error);
    reset_value(&c.selected);
    reset_option_states(c);
    close_popup();
    if (!replacement.bytes)
        return composer_apply_result(message_composer_clear(&g_app.message_composer), true);
    const auto result =
        message_composer_replace_range(&g_app.message_composer, 0, g_app.message_composer.bytes, replacement);
    return result == MessageComposerResult::invalid_input || composer_apply_result(result, true);
}
bool application_commands_results_refused() noexcept {
    if (!commands || !commands->selected.command)
        return true;
    commands->error = application_media_picker_inline_message();
    shake_app();
    return render() && refresh_message_composer_ui();
}
// The pointer took over: hovering a row drops the keyboard
// selection (and the keyboard's place in the rail).
void application_commands_hover() noexcept {
    if (commands) {
        commands->keyboard_row = no_row;
        commands->rail_focus = no_row;
    }
}
u32 application_commands_icon_url(u64 media_id, Slice<char> url) noexcept {
    if (!commands || !media_id)
        return 0;
    for (const auto& entry : commands->icon_urls)
        if (entry.hash == media_id && entry.bytes && entry.bytes <= url.count) {
            copy_bytes(url.data, entry.url, entry.bytes);
            return entry.bytes;
        }
    return 0;
}
void application_commands_dismiss() noexcept {
    if (commands) {
        command_autocomplete_cancel(&commands->autocomplete);
        commands->dismissed_document = g_app.message_composer.document_identity;
        commands->dismissed_revision = g_app.message_composer.text_revision;
    }
}
bool application_commands_escape() noexcept {
    const auto& composer = g_app.message_composer;
    if (!command_in_draft() || commands->selected.index != builtin_commands_index() ||
        (commands->dismissed_document == composer.document_identity &&
         commands->dismissed_revision == composer.text_revision))
        return false;
    application_commands_dismiss();
    // The sync closes the built-in's inline results while dismissed.
    refresh_message_composer_ui();
    return true;
}
void application_commands_gateway(Utf8View body) noexcept {
    if (!commands || !g_app.account_ui || !g_app.popup_scene.active ||
        g_app.popup_owner_kind != PopupOwnerKind::commands)
        return;
    // The Gateway pump still owns its payload scratch. Delay UI work until
    // the drain completes; only the retained suggestion result survives here.
    auto& c = *commands;
    c.suggestions_dirty = command_autocomplete_accept(&c.autocomplete, body,
                                                      {g_app.account_ui->message_history_page_structurals,
                                                       kMessageHistoryPageStructuralCount}) ||
                          c.suggestions_dirty;
}
namespace {
// An index is going: a selection pointing into it ends, the picker's projection
// (which borrows every index) goes and the catalog is published again.
void drop_index(Commands& c, const CommandIndex* index) noexcept {
    if (c.selected.index == index) {
        reset_value(&c.selected);
        reset_value(&c.focused_option);
        c.choice_count = c.option_count = 0;
        command_autocomplete_cancel(&c.autocomplete);
    }
    arena_release(&c.projection);
    c.entries = nullptr;
    c.rail = c.body = nullptr;
    c.count = c.rail_count = c.body_count = 0;
    c.filter_app = 0;
    c.revision = 0;
    c.catalog_dirty = true;
    ++c.catalog_revision;
    ++c.generation;
}
} // namespace
void application_commands_invalidate_guild(u64 guild_id) noexcept {
    if (!guild_id || !commands || !commands->channel || (commands->guild && commands->guild != guild_id))
        return;
    auto& c = *commands;
    advance_channel_index_epoch(c);
    c.loaded[0] = false;
    c.pending |= 1;
    c.attempts[0] = 0;
    if (c.indexing_retry && c.requested_guild == c.guild && !c.queued && !c.sending) {
        c.retry_at = 0;
        c.indexing_retry = false;
    }
    drop_index(c, &c.index[0]);
    // Visible context-menu labels can still borrow this arena. Retire it until
    // post-drain UI refresh, while making the stale commands unselectable now.
    if (c.index[0].storage.base)
        c.retired_index_storage = c.index[0].storage;
    reset_value(&c.index[0]);
    // This close is a cache refresh, not a dismissal of the unchanged draft.
    hide_popup();
}
bool application_commands_tick(u64 now) noexcept {
    if (!commands)
        return true;
    auto& c = *commands;
    shake_step(now);
    // Plugins were enabled or disabled: what points into their index goes, then the index is rebuilt.
    if (plugin_commands_changed()) {
        drop_index(c, plugin_commands_storage());
        plugin_commands_refresh();
    }
    // A /msg or /thread message goes out once its channel is there.
    if (!builtin_commands_tick())
        return false;
    if (c.echo_dirty) {
        c.echo_dirty = false;
        if (!refresh_account_frame(true))
            return false;
    }
    if (c.catalog_dirty) {
        c.catalog_dirty = false;
        application_user_actions_commands_changed();
        // A failed/hidden context-menu refresh must not leave borrowed text
        // visible after the retired index storage is released.
        for (u32 n = 0; g_app.popup_scene.active && n < g_app.popup_scene.item_count; ++n) {
            const auto& item = g_app.popup_scene.items[n];
            if (retired_text(item.label, c.retired_index_storage) ||
                retired_text(item.detail, c.retired_index_storage)) {
                popup_scene_close(&g_app.popup_scene);
                set_popup_owner(PopupOwnerKind::none);
            }
        }
        arena_release(&c.retired_index_storage);
        if (!refresh_message_composer_ui())
            return false;
    }
    if (!g_app.popup_scene.active || g_app.popup_owner_kind != PopupOwnerKind::commands) {
        command_autocomplete_cancel(&c.autocomplete);
        c.suggestions_dirty = false;
        return true;
    }
    c.suggestions_dirty = command_autocomplete_tick(&c.autocomplete, now) || c.suggestions_dirty;
    if (!c.suggestions_dirty)
        return true;
    c.suggestions_dirty = false;
    ++c.generation;
    return g_app.popup_owner_kind != PopupOwnerKind::commands || refresh_message_composer_ui();
}
u64 application_commands_next_wake() noexcept {
    if (!commands)
        return 0;
    const auto& c = *commands;
    if (c.catalog_dirty || c.echo_dirty)
        return 1;
    const bool idle = g_app.details_request_owner == MessageHistoryRequestOwner::none;
    u64 wake = c.autocomplete.phase == CommandAutocompletePhase::waiting ? c.autocomplete.deadline : 0;
    if (c.shake_started)
        wake = earliest_deadline(wake, maximum<u64>(c.shake_next, 1));
    if (!idle)
        return wake;
    if (c.suggestions_dirty)
        return 1;
    if (builtin_commands_pending() && !c.sending)
        wake = earliest_deadline(wake, c.retry_at ? c.retry_at : 1);
    if (c.autocomplete.phase == CommandAutocompletePhase::debounce)
        wake = earliest_deadline(wake, maximum(c.autocomplete.due_at, c.retry_at));
    // One deadline each for a wanted frecency fetch and the pending save.
    if (const auto* f = frecency; f && !c.sending && frecency_ready()) {
        if (!frecency_current() && (g_app.popup_owner_kind == PopupOwnerKind::commands || f->pending_count))
            wake = earliest_deadline(wake, maximum<u64>(maximum(f->retry_after, c.retry_at), 1));
        else if (frecency_current() && f->pending_count && f->save_due)
            wake = earliest_deadline(wake, maximum(maximum(f->save_due, f->retry_after), c.retry_at));
    }
    if (c.sending ||
        (!c.queued && (!c.pending || (g_app.popup_owner_kind != PopupOwnerKind::commands && !c.context_wanted &&
                                      !command_in_draft()))))
        return wake;
    return earliest_deadline(wake, c.retry_at ? c.retry_at : 1);
}

namespace {

// Binds the command index to the active channel without disturbing a draft.
bool bind_context_channel() noexcept {
    if (!g_app.account_ui || !ensure_commands())
        return false;
    auto& c = *commands;
    const u64 channel_id = g_app.account_ui->active_channel_id;
    if (!channel_id)
        return false;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    const u64 guild = channel ? channel->guild_id : 0;
    if (c.channel != channel_id) {
        if (c.queued || c.sending)
            return false;
        advance_channel_index_epoch(c);
        if (c.guild != guild || !guild) {
            c.loaded[0] = false;
            if (c.selected.index == &c.index[0])
                reset_value(&c.selected);
            command_index_release(&c.index[0]);
            c.attempts[0] = 0;
            if (!project({}))
                return false;
        }
        c.channel = channel_id;
        c.guild = guild;
        c.filter_app = 0;
        c.revision = 0;
    }
    if (!c.loaded[0])
        c.pending |= 1;
    if (!c.loaded[1])
        c.pending |= 2;
    if (c.pending)
        c.context_wanted = true;
    return true;
}

} // namespace

u32 application_commands_context(u8 type, Slice<ApplicationContextCommand> output, bool* loading) noexcept {
    if (loading)
        *loading = false;
    if (!bind_context_channel() || !output.data)
        return 0;
    auto& c = *commands;
    if (loading)
        *loading = c.pending != 0 && (!c.loaded[0] || !c.loaded[1]);
    u32 count = 0;
    for (const auto& source : c.index)
        for (u32 n = 0; n < source.context_command_count && count < output.count; ++n) {
            const auto& command = source.context_commands[n];
            if (source.context_types[n] != type || (command.guild_id && command.guild_id != c.guild) ||
                (!c.guild && !command.dm_permission))
                continue;
            bool duplicate = false;
            for (u32 j = 0; j < count && !duplicate; ++j)
                duplicate = output[j].command_id == command.id;
            if (duplicate)
                continue;
            const auto* app = find_app(source, command.application_id);
            output[count++] = {command.name, app ? app->name : Utf8View{}, command.id, command.application_id};
        }
    return count;
}
bool application_commands_context_invoke(u8 type, u64 command_id, u64 target_id) noexcept {
    if (!target_id || !command_id || !bind_context_channel() ||
        g_app.account.status.required_action != AccountRequirement::none)
        return false;
    auto& c = *commands;
    if (c.sending || c.queued)
        return false;
    const ApplicationCommand* found = nullptr;
    const CommandApplication* app = nullptr;
    for (const auto& source : c.index)
        for (u32 n = 0; n < source.context_command_count && !found; ++n)
            if (source.context_commands[n].id == command_id && source.context_types[n] == type) {
                found = &source.context_commands[n];
                app = find_app(source, found->application_id);
            }
    if (!found)
        return false;
    ProtocolWriter data{{reinterpret_cast<u8*>(c.payload), sizeof(c.payload)}, 0};
    if (!command_context_data(*found, type, target_id, &data))
        return false;
    c.payload_bytes = data.used;
    frecency_track(*found, c.guild);
    queue_command(found->application_id, echo_author(found->application_id, app), found->name,
                  message_type_context_command);
    if (g_app.window)
        window_wake(g_app.window);
    return true;
}
bool application_commands_retry(u64 message_id) noexcept {
    if (!commands || !message_id || !g_app.account_ui)
        return false;
    Echo* echo = find_echo(message_id, 0);
    if (!echo)
        return false;
    auto& c = *commands;
    // A command lane in use or account action pending keeps the failed row for later.
    if (!echo->failed || !echo->data_bytes || c.queued || c.sending || echo->channel != c.channel ||
        g_app.account.status.required_action != AccountRequirement::none)
        return true;
    // The failed local message goes and the same command data
    // is executed again with a new nonce and a new local message.
    copy_bytes(c.payload, echo->data, echo->data_bytes);
    c.payload_bytes = echo->data_bytes;
    const u64 application = echo->application, author = echo->author;
    const u16 type = echo->type;
    char name[sizeof(echo->name)];
    const u32 name_bytes = echo->name_bytes;
    copy_bytes(name, echo->name, name_bytes);
    echo_remove(*echo);
    queue_command(application, author, {name, name_bytes}, type);
    if (g_app.window)
        window_wake(g_app.window);
    return true;
}
void application_commands_interaction(GatewayClientDispatchKind kind, Utf8View body) noexcept {
    if (!commands || !g_app.account_ui)
        return;
    MessageInteractionEvent event{};
    if (!message_interaction_event_decode(
            body, {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount}, &event))
        return;
    Echo* echo = find_echo(event.nonce, event.id);
    if (!echo)
        return;
    switch (kind) {
    case GatewayClientDispatchKind::interaction_create:
        if (echo->phase == EchoPhase::sending) {
            echo->interaction_id = event.id;
            echo->phase = EchoPhase::created;
            echo_write(*echo, {}, message_flag_loading, MessageDeliveryState::pending);
        }
        break;
    case GatewayClientDispatchKind::interaction_success:
        // The response message replaces the echo; until then it shows no status.
        if (echo->phase != EchoPhase::settled) {
            echo->phase = EchoPhase::settled;
            echo_write(*echo, {}, 0, MessageDeliveryState::pending);
        }
        break;
    case GatewayClientDispatchKind::interaction_failure:
        if (echo->phase != EchoPhase::settled)
            echo_fail(*echo, {});
        break;
    case GatewayClientDispatchKind::interaction_modal_create:
        echo_remove(*echo);
        break;
    default:
        break;
    }
}
void application_commands_message_created(u64 nonce) noexcept {
    if (!commands || !nonce)
        return;
    if (Echo* echo = find_echo(nonce, 0))
        echo_remove(*echo);
}
u32 application_commands_for_application(u64 channel_id, u64 application_id, Slice<ApplicationRootCommand> output,
                                         bool* loading) noexcept {
    if (loading)
        *loading = false;
    if (!channel_id || !application_id || !output.data || !g_app.account_ui ||
        g_app.account_ui->active_channel_id != channel_id || !bind_context_channel())
        return 0;
    auto& c = *commands;
    if (loading)
        *loading = c.pending != 0 && (!c.loaded[0] || !c.loaded[1]);
    if (!commands_permitted(c))
        return 0;
    u32 count = 0;
    // The user-installed index wins over the channel's for a shared command.
    for (u32 slot = 2; slot-- > 0;) {
        const auto& source = c.index[slot];
        for (u32 n = 0; n < source.command_count; ++n) {
            const auto& command = source.commands[n];
            if (command.application_id != application_id || (!c.guild && !command.dm_permission) ||
                (command.guild_id && command.guild_id != c.guild))
                continue;
            const bool nested = command.group_name.bytes || command.sub_name.bytes;
            u32 at = 0;
            while (at < count && output[at].command_id != command.id)
                ++at;
            if (at < count) {
                if (nested) {
                    output[at].subcommands = true;
                    reset_value(&output[at].description);
                }
                continue;
            }
            if (count == output.count)
                continue;
            output[count++] = {command.root_name, nested ? Utf8View{} : command.description, command.id,
                               command.nsfw, nested};
        }
    }
    return count;
}
u32 application_commands_catalog_revision() noexcept {
    return commands ? commands->catalog_revision : 0;
}

namespace {

TextFormOwnerKey insert_confirmation;
// Fills "/<name> " for the stored insert target, selecting it at once when loaded.
bool insert_now() noexcept {
    auto& c = *commands;
    const Utf8View name{c.insert_name, c.insert_name_bytes};
    reset_value(&c.selected);
    c.insert_wanted = true;
    Entry entry{};
    if (c.channel == g_app.message_composer.channel_id &&
        find_entry(c.insert_application, c.insert_command, name, &entry)) {
        c.selected = entry;
        c.insert_wanted = false;
        c.revision = 0;
        c.offset = 0;
    }
    return application_composer_fill_command(name);
}
bool insert_confirmation_valid(TextFormOwnerKey key) noexcept {
    return commands && g_app.account_ui && key.scope == g_app.plugin_account_scope_serial &&
           key.scope == insert_confirmation.scope && key.collection == insert_confirmation.collection &&
           key.item == insert_confirmation.item && key.collection == g_app.account_ui->active_channel_id;
}
bool insert_confirmation_save(TextFormOwnerKey key, Utf8View) noexcept {
    if (!insert_confirmation_valid(key))
        return true;
    reset_value(&insert_confirmation);
    return insert_now();
}

} // namespace

bool application_commands_insert(u64 application_id, u64 command_id, Utf8View name) noexcept {
    if (!command_id || !name.bytes || name.bytes >= sizeof(Commands::insert_name) || !g_app.account_ui ||
        !ensure_commands())
        return false;
    auto& c = *commands;
    c.insert_application = application_id;
    c.insert_command = command_id;
    c.insert_name_bytes = utf8_copy_truncated(c.insert_name, sizeof(c.insert_name), name);
    if (!g_app.message_composer.bytes)
        return insert_now();
    // Replacing a written draft asks first.
    insert_confirmation = {g_app.plugin_account_scope_serial, g_app.account_ui->active_channel_id, command_id};
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Clear message?");
    request.context = utf8_literal(
        "Looks like you\xE2\x80\x99ve already written something. Do you want to clear it and use a command?");
    request.accept = utf8_literal("Clear");
    request.owner = insert_confirmation;
    request.valid = insert_confirmation_valid;
    request.save = insert_confirmation_save;
    request.confirmation_only = true;
    if (!application_text_form_open(request))
        reset_value(&insert_confirmation);
    return true;
}
void application_commands_release() noexcept {
    builtin_commands_release();
    plugin_commands_release();
    if (!commands)
        return;
    command_autocomplete_cancel(&commands->autocomplete);
    command_index_release(&commands->index[0]);
    command_index_release(&commands->index[1]);
    arena_release(&commands->projection);
    arena_release(&commands->retired_index_storage);
    pages_release(commands);
    commands = nullptr;
    if (frecency)
        pages_release(frecency);
    frecency = nullptr;
}

} // namespace maicord::app_detail
