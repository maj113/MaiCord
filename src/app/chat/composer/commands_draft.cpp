#include "commands_internal.hpp"
#include "../messages/components.hpp"
#include "maicord/core/log.hpp"
#include "../../account/account_access.hpp"

namespace maicord::app_detail {
namespace commands_detail {

bool render() noexcept {
    auto& c = *commands;
    const auto* surface = ui_widget_manager_layout_node(g_app.message_composer_scene.widgets,
                                                         g_app.message_composer_scene.surface);
    if (!surface)
        return true;
    return c.selected.command ? render_command(*surface) : render_picker(*surface);
}

Echo* find_echo(u64 nonce, u64 interaction_id) noexcept {
    for (auto& echo : commands->echoes)
        if (echo.phase != EchoPhase::free &&
            ((nonce && echo.nonce == nonce) || (interaction_id && echo.interaction_id == interaction_id)))
            return &echo;
    return nullptr;
}
// Rewrites the echo's local record (it stays at its nonce's place in time).
void echo_write(Echo& echo, Utf8View content, u32 flags, MessageDeliveryState delivery) noexcept {
    auto* timeline = &g_app.message_view.timeline;
    commands->echo_dirty = true;
    if (!g_app.account_ui || g_app.account_ui->active_channel_id != echo.channel ||
        !message_timeline_remove_local(timeline, echo.nonce).ok()) {
        echo.phase = EchoPhase::free;
        return;
    }
    MessageInput input{};
    input.id = echo.nonce;
    input.channel_id = echo.channel;
    input.author_id = echo.author;
    input.content = content;
    input.flags = flags;
    input.delivery = delivery;
    input.type = echo.type;
    if (!message_timeline_insert_local(timeline, input).ok()) {
        echo.phase = EchoPhase::free;
        return;
    }
    const auto& users = g_app.account_ui->user_store;
    const u64 self = g_app.account_ui->chat_state.self_user_id;
    const auto* user = user_store_find(users, self);
    GatewayMessageInteraction interaction{};
    interaction.id = echo.interaction_id;
    interaction.user_id = self;
    interaction.command_name = {echo.name, echo.name_bytes};
    interaction.type = 2;
    if (user) {
        interaction.user_name = user_store_name(users, *user);
        interaction.user_avatar_hash = user_store_avatar_hash(*user);
    }
    if (!message_timeline_set_local_interaction(timeline, echo.nonce, interaction))
        log_write(LogLevel::warning, "commands", "echo_header_failed");
}
// The echo's author: the app's bot (or the app itself as a placeholder bot),
// made resident so the row shows its name and avatar.
u64 echo_author(u64 application_id, const CommandApplication* app) noexcept {
    const u64 author = app && app->bot_id ? app->bot_id : application_id;
    auto& users = g_app.account_ui->user_store;
    const Utf8View name = app ? (app->bot_name.bytes ? app->bot_name : app->name) : Utf8View{};
    if (!user_store_find(users, author) && name.bytes && user_store_upsert(&users, author, name).ok() &&
        app->avatar.bytes)
        user_store_set_avatar(&users, author, app->avatar);
    return author;
}
// Queues the command data already in c.payload on the command lane with a
// fresh nonce, expects its modal and shows the local
// "Sending command..." message when its channel is visible.
void queue_command(u64 application, u64 author, Utf8View name, u16 type) noexcept {
    auto& c = *commands;
    c.queued = true;
    c.send_application = application;
    c.send_channel = c.channel;
    c.send_guild = c.guild;
    c.nonce = next_nonce();
    application_message_components_expect_modal(c.nonce, application);
    if (!g_app.account_ui || g_app.account_ui->active_channel_id != c.channel)
        return;
    auto& echo = c.echoes[c.echo_next++ % echo_slots];
    // Only the header is reset; data pages are touched as far as the command needs.
    echo.interaction_id = 0;
    echo.failed = false;
    echo.nonce = c.nonce;
    echo.channel = c.channel;
    echo.guild = c.guild;
    echo.application = application;
    echo.type = type;
    echo.author = author;
    echo.name_bytes = static_cast<u8>(utf8_copy_truncated(echo.name, sizeof(echo.name), name));
    echo.data_bytes = c.payload_bytes;
    copy_bytes(echo.data, c.payload, c.payload_bytes);
    echo.phase = EchoPhase::sending;
    echo_write(echo, echo_sending, 0, MessageDeliveryState::pending);
}
// A quick refusal shows the API's reason, and "did not respond" once the
// response window has passed.
void echo_fail(Echo& echo, Utf8View reason) noexcept {
    const u64 sent = snowflake_timestamp_ms(echo.nonce);
    const Utf8View text = clock_unix_milliseconds() > sent + echo_response_window_ms ? echo_no_response
                          : reason.bytes                                              ? reason
                                                                                      : echo_failed;
    echo.phase = EchoPhase::settled;
    echo.failed = true;
    echo_write(echo, text, 0, MessageDeliveryState::failed);
}
void echo_remove(Echo& echo) noexcept {
    if (g_app.account_ui && g_app.account_ui->active_channel_id == echo.channel &&
        message_timeline_remove_local(&g_app.message_view.timeline, echo.nonce).ok())
        commands->echo_dirty = true;
    echo.phase = EchoPhase::free;
}
// A refused stale command: refetch the indexes the next time they are wanted.
void mark_indexes_stale(Commands& c) noexcept {
    for (u8 slot = 0; slot < 2; ++slot) {
        c.loaded[slot] = false;
        c.pending |= static_cast<u8>(1u << slot);
        c.attempts[slot] = 0;
    }
}
// Entry for a command picked by id outside the picker (a profile chip or a
// command mention): the id, its app when known, and the full name must match.
bool find_entry(u64 application_id, u64 command_id, Utf8View name, Entry* output) noexcept {
    for (const auto& source : commands->index)
        for (u32 n = 0; n < source.command_count; ++n) {
            const auto& command = source.commands[n];
            if (command.id == command_id && (!application_id || command.application_id == application_id) &&
                utf8_equal(command.name, name)) {
                *output = {&command, &source, find_app(source, command.application_id)};
                return true;
            }
        }
    if (const CommandIndex* plugins = plugin_commands_index())
        for (u32 n = 0; n < plugins->command_count; ++n) {
            const auto& command = plugins->commands[n];
            if (command.id == command_id && (!application_id || command.application_id == application_id) &&
                utf8_equal(command.name, name)) {
                *output = {&command, plugins, find_app(*plugins, command.application_id)};
                return true;
            }
        }
    return false;
}

} // namespace commands_detail
namespace {

struct AttachmentProbe {
    const CommandIndex* index;
    bool found;
};

} // namespace
namespace commands_detail {

bool has_attachment_option(const CommandIndex& index, const ApplicationCommand& command) noexcept {
    AttachmentProbe probe{&index, false};
    if (command.options.source.bytes)
        json_visit_indexed_array(
            index.document, command.options, 64,
            [](void* opaque, u32, JsonValue option) noexcept {
                auto& p = *static_cast<AttachmentProbe*>(opaque);
                p.found = p.found || option_type(*p.index, option) == 11;
                return true;
            },
            &probe);
    return probe.found;
}
// Command permission in the bound channel (Use Application Commands).
bool commands_permitted(const Commands& c) noexcept {
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, c.channel);
    return !channel || !channel->permissions_resolved || (channel->effective_permissions & (1ull << 31));
}
// Selects a command picked outside the picker once its index is loaded, while
// the draft still starts with its name; dropped once both indexes lack it.
void resolve_wanted(Utf8View input) noexcept {
    auto& c = *commands;
    if (!c.insert_wanted || c.selected.command || c.channel != g_app.message_composer.channel_id)
        return;
    Entry wanted{};
    if (find_entry(c.insert_application, c.insert_command, {c.insert_name, c.insert_name_bytes}, &wanted)) {
        const auto name = wanted.command->name;
        if (input.bytes > name.bytes && input.data[0] == '/' && utf8_equal({input.data + 1, name.bytes}, name) &&
            (input.bytes == name.bytes + 1 || command_space(input.data[name.bytes + 1]))) {
            c.selected = wanted;
            c.revision = 0;
            c.offset = 0;
        }
        c.insert_wanted = false;
    } else if (c.loaded[0] && c.loaded[1]) {
        c.insert_wanted = false;
    }
}

} // namespace commands_detail
namespace {

Utf8View trimmed_text(Utf8View input, u32 first, u32 last) noexcept {
    while (first < last && command_space(input.data[first]))
        ++first;
    while (last > first && command_space(input.data[last - 1]))
        --last;
    return {input.data + first, last - first};
}

} // namespace
namespace commands_detail {

// A typed command: with none selected, a draft of
// "/<name> " selects the one available command of exactly that name, unless a
// longer command (a subcommand) continues it.
bool select_typed(Utf8View input) noexcept {
    auto& c = *commands;
    if (input.bytes < 3 || input.data[0] != '/' || input.data[input.bytes - 1] != ' ')
        return false;
    const Utf8View name = trimmed_text(input, 1, input.bytes);
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, c.channel);
    const bool permitted =
        !channel || !channel->permissions_resolved || (channel->effective_permissions & (1ull << 31));
    Entry found{};
    u32 matches = 0;
    const auto consider = [&](const ApplicationCommand& command, const CommandIndex& source) noexcept {
        const auto candidate = command.name;
        if (candidate.bytes > name.bytes && command_space(candidate.data[name.bytes]) &&
            utf8_equal({candidate.data, name.bytes}, name))
            matches += 2;
        else if (utf8_equal(candidate, name) && (!found.command || found.command->id != command.id)) {
            found = {&command, &source, find_app(source, command.application_id)};
            ++matches;
        }
    };
    for (const auto& source : c.index)
        for (u32 n = 0; permitted && n < source.command_count; ++n) {
            const auto& command = source.commands[n];
            if ((!c.guild && !command.dm_permission) || (command.guild_id && command.guild_id != c.guild))
                continue;
            consider(command, source);
        }
    const CommandIndex* builtins = builtin_commands_index();
    for (u32 n = 0; builtins && n < builtins->command_count; ++n)
        if (builtin_command_available(builtins->commands[n], c.channel))
            consider(builtins->commands[n], *builtins);
    // A plugin's commands need no permission of the channel and no guild.
    const CommandIndex* plugins = plugin_commands_index();
    for (u32 n = 0; plugins && n < plugins->command_count; ++n)
        if (c.guild || plugins->commands[n].dm_permission)
            consider(plugins->commands[n], *plugins);
    if (matches != 1)
        return false;
    c.selected = found;
    c.offset = 0;
    return true;
}

} // namespace commands_detail
namespace {

// Puts text in the draft and the caret at caret.
MessageComposerResult rewrite_draft(Utf8View text, u32 caret) noexcept {
    const auto input = message_composer_text(g_app.message_composer);
    const auto replaced = utf8_equal(input, text)
                              ? MessageComposerResult::unchanged
                              : message_composer_replace_range(&g_app.message_composer, 0, input.bytes, text);
    if (replaced == MessageComposerResult::invalid_input)
        return replaced;
    const auto selected = message_composer_set_selection(&g_app.message_composer, caret, caret);
    return replaced == MessageComposerResult::changed ? replaced : selected;
}

} // namespace
namespace commands_detail {

// The draft's option end markers (kCommandOptionEnd), the plain-text form of
// option pill nodes: each chip value gets one at its
// end, so text typed after it is outside the chip and "name:" typed inside it
// stays text; markers ending no value go, as do all without a command. A
// marker added where the user types joins their typing run, so one undo takes
// both; nothing changes while a redo is pending, so undo never fights it.
MessageComposerResult normalize_markers(Utf8View input, bool command) noexcept {
    auto& composer = g_app.message_composer;
    if (message_composer_history_costs(composer).redo_steps)
        return MessageComposerResult::unchanged;
    CommandArgumentSpan spans[25];
    const u32 count = command ? typed_options(input, {spans, 25}) : 0;
    u32 inserts[25], insert_count = 0, removals = 0;
    for (u32 n = 0; n < count; ++n) {
        if (spans[n].end_last != spans[n].value_last)
            continue;
        u32 end = spans[n].value_last;
        while (end > spans[n].value_first && command_space(input.data[end - 1]))
            --end;
        inserts[insert_count++] = end;
    }
    const auto stray = [&](u32 at) noexcept {
        if (!option_end_at(input, at))
            return false;
        for (u32 n = 0; n < count; ++n)
            if (spans[n].end_last != spans[n].value_last && spans[n].value_last == at)
                return false;
        return true;
    };
    for (u32 at = 0; at < input.bytes; ++at)
        if (stray(at)) {
            ++removals;
            at += kCommandOptionEndBytes - 1;
        }
    if (!insert_count && !removals)
        return MessageComposerResult::unchanged;
    const u32 anchor = composer.anchor, caret = composer.caret;
    if (!removals && insert_count == 1 && anchor == caret && caret == inserts[0]) {
        const auto inserted = message_composer_insert_scalar(&composer, 0x2063, clock_monotonic_milliseconds());
        if (inserted == MessageComposerResult::changed)
            message_composer_set_selection(&composer, caret, caret);
        return inserted;
    }
    // A caret at a new marker stays before it (in the value); one in a removed
    // marker goes to its place.
    char text[kMessageComposerMaximumBytes];
    u32 used = 0, next = 0, new_anchor = 0, new_caret = 0;
    for (u32 at = 0;;) {
        if (at == anchor)
            new_anchor = used;
        if (at == caret)
            new_caret = used;
        for (; next < insert_count && inserts[next] == at; ++next) {
            if (used + kCommandOptionEndBytes > sizeof(text))
                return MessageComposerResult::unchanged;
            copy_bytes(text + used, kCommandOptionEnd, kCommandOptionEndBytes);
            used += kCommandOptionEndBytes;
        }
        if (at >= input.bytes)
            break;
        if (stray(at)) {
            if (anchor > at && anchor < at + kCommandOptionEndBytes)
                new_anchor = used;
            if (caret > at && caret < at + kCommandOptionEndBytes)
                new_caret = used;
            at += kCommandOptionEndBytes;
            continue;
        }
        if (used == sizeof(text))
            return MessageComposerResult::unchanged;
        text[used++] = input.data[at++];
    }
    const auto replaced = message_composer_replace_range(&composer, 0, input.bytes, {text, used});
    if (replaced == MessageComposerResult::changed)
        message_composer_set_selection(&composer, new_anchor, new_caret);
    return replaced;
}
// The single optional option: with no chip yet, text typed
// after "/<name> " becomes the value of a command's only option when that is
// optional and not an ATTACHMENT.
MessageComposerResult chip_single_option(Utf8View input, u32 typed) noexcept {
    const auto& c = *commands;
    const auto list = command_options();
    const u32 start = c.selected.command->name.bytes + 2;
    if (typed || list.count != 1 || option_flag(list.options[0], "required") ||
        option_type(*c.selected.index, list.options[0]) == 11 || input.bytes <= start ||
        !command_space(input.data[start - 1]))
        return MessageComposerResult::unchanged;
    char key[130];
    const auto name = field_text(*c.selected.index, list.options[0], "name", {key, 128});
    if (!name.bytes)
        return MessageComposerResult::unchanged;
    key[name.bytes] = ':';
    const u32 caret = g_app.message_composer.caret;
    const auto inserted = message_composer_replace_range(&g_app.message_composer, start, start,
                                                         {key, name.bytes + 1});
    if (inserted != MessageComposerResult::changed)
        return inserted;
    const u32 moved = caret >= start ? caret + name.bytes + 1 : caret;
    message_composer_set_selection(&g_app.message_composer, moved, moved);
    return inserted;
}
// Command set-up: the typed
// options stay, every required option without one follows as an empty chip
// (text after the name fills the first one that is not an ATTACHMENT), and the
// caret goes into the first empty chip, else to the end of the last chip's
// value, else to the end; each value ends at its end marker. Every option is
// then checked with empty values allowed.
MessageComposerResult set_up_command() noexcept {
    auto& c = *commands;
    c.prepared = c.selected.command;
    reset_option_states(c);
    const auto input = message_composer_text(g_app.message_composer);
    const auto name = c.selected.command->name;
    CommandArgumentSpan spans[25];
    const u32 typed = typed_options(input, {spans, 25});
    Utf8View leftover = trimmed_text(input, minimum(input.bytes, name.bytes + 1), typed ? spans[0].key_first : input.bytes);
    const auto list = command_options();
    struct Added {
        u32 slot;
        Utf8View value;
    } added[25];
    u32 added_count = 0;
    for (u32 slot = 0; slot < list.count; ++slot) {
        bool present = false;
        for (u32 n = 0; n < typed && !present; ++n)
            present = spans[n].slot == slot;
        if (present || !option_flag(list.options[slot], "required"))
            continue;
        added[added_count] = {slot, {}};
        if (leftover.bytes && option_type(*c.selected.index, list.options[slot]) != 11) {
            added[added_count].value = leftover;
            leftover = {};
        }
        ++added_count;
    }
    char text[kMessageComposerMaximumBytes];
    ProtocolWriter w{{reinterpret_cast<u8*>(text), sizeof(text)}, 0};
    const auto append = [&](Utf8View part) noexcept {
        return protocol_writer_append_bytes(&w, {reinterpret_cast<const u8*>(part.data), part.bytes});
    };
    bool ok = protocol_writer_append_byte(&w, '/') && append(name);
    if (leftover.bytes) {
        ok = ok && protocol_writer_append_byte(&w, ' ');
        // The command's text node is one line.
        for (u32 n = 0; ok && n < leftover.bytes; ++n)
            ok = protocol_writer_append_byte(
                &w, leftover.data[n] == '\r' || leftover.data[n] == '\n' ? ' ' : leftover.data[n]);
    }
    const Utf8View end_marker{kCommandOptionEnd, kCommandOptionEndBytes};
    u32 caret = no_option, last_value = no_option;
    for (u32 n = 0; ok && n < typed; ++n) {
        ok = protocol_writer_append_byte(&w, ' ') && append(spans[n].name) && protocol_writer_append_byte(&w, ':');
        if (!spans[n].value.bytes && caret == no_option)
            caret = w.used;
        ok = ok && append(spans[n].value);
        last_value = w.used;
        ok = ok && append(end_marker);
    }
    // Text typed after the last chip stays outside it.
    const Utf8View tail = typed ? trimmed_text(input, spans[typed - 1].end_last, input.bytes) : Utf8View{};
    if (tail.bytes) {
        ok = ok && protocol_writer_append_byte(&w, ' ') && append(tail);
        last_value = no_option;
    }
    for (u32 n = 0; ok && n < added_count; ++n) {
        char storage[128];
        ok = protocol_writer_append_byte(&w, ' ') &&
             append(field_text(*c.selected.index, list.options[added[n].slot], "name", {storage, sizeof(storage)})) &&
             protocol_writer_append_byte(&w, ':');
        if (!added[n].value.bytes && caret == no_option)
            caret = w.used;
        ok = ok && append(added[n].value);
        last_value = w.used;
        ok = ok && append(end_marker);
    }
    if (ok && !typed && !added_count && !leftover.bytes)
        ok = protocol_writer_append_byte(&w, ' ');
    if (!ok || w.used > kMessageComposerMaximumBytes)
        return MessageComposerResult::unchanged;
    const auto result =
        rewrite_draft({text, w.used}, caret != no_option ? caret : last_value != no_option ? last_value : w.used);
    const auto now = message_composer_text(g_app.message_composer);
    const u32 count = typed_options(now, {spans, 25});
    const u32 chip = chip_at(now, spans, count, g_app.message_composer.caret);
    check_options(spans, count, chip == no_option ? no_option : spans[chip].slot, true, true);
    if (added_count)
        return result;
    const auto single = chip_single_option(now, count);
    return single == MessageComposerResult::changed ? single : result;
}
// Selects a chip's whole value.
MessageComposerResult select_chip(const CommandArgumentSpan& span) noexcept {
    return message_composer_set_selection(&g_app.message_composer, span.value_first, span.value_last);
}

} // namespace commands_detail
namespace {

// The app shake on a refused send (duration 200 ms, intensity 2): on every
// other animation frame the
// whole app moves intensity DIPs across, the side alternating, and a random
// amount within ±intensity up or down, until the duration has passed. MaiCord
// moves the window's composition visual, so no frame is drawn again; motion
// preferences leave it still.
constexpr u64 shake_duration_ms = 200, shake_frame_ms = 16;
constexpr float shake_intensity = 2;
bool shake_motion() noexcept {
    return !g_app.preferences.reduced_motion && !g_app.renderer.reduced_motion &&
           (!g_app.surface_motion.system_preference_cached || g_app.surface_motion.client_area_animation);
}

} // namespace
namespace commands_detail {

void shake_app() noexcept {
    auto& c = *commands;
    if (!shake_motion())
        return;
    const u64 now = clock_monotonic_milliseconds();
    c.shake_started = now ? now : 1;
    c.shake_next = c.shake_started;
    c.shake_frame = 0;
    c.shake_direction = -1;
    c.shake_seed = static_cast<u32>(now) | 1;
    application_request_frame();
}
// One shake frame when due; the content returns to rest at the end.
void shake_step(u64 now) noexcept {
    auto& c = *commands;
    if (!c.shake_started || now < c.shake_next)
        return;
    const float scale = static_cast<float>(maximum(g_app.renderer.dpi, 1u)) / 96.f;
    if (now - c.shake_started > shake_duration_ms || !shake_motion()) {
        d3d11_device_owner_set_offset(&g_app.renderer.graphics, 0, 0);
        c.shake_started = c.shake_next = 0;
        return;
    }
    if (c.shake_frame++ % 2) {
        c.shake_seed = c.shake_seed * 1664525u + 1013904223u;
        const float random = static_cast<float>(c.shake_seed >> 8) / static_cast<float>(1u << 24) * 2 - 1;
        d3d11_device_owner_set_offset(&g_app.renderer.graphics, c.shake_direction * shake_intensity * scale,
                                      random * shake_intensity * scale);
        c.shake_direction = -c.shake_direction;
    }
    c.shake_next = now + shake_frame_ms;
}
// The send check: every option is checked with empty
// values refused, required options without a chip are added as empty chips
// at the end, and the first failing chip (typed order, then the added ones)
// is selected instead of sending. *blocked tells the send stopped.
bool send_check(bool* blocked) noexcept {
    auto& c = *commands;
    *blocked = false;
    const auto input = message_composer_text(g_app.message_composer);
    CommandArgumentSpan spans[25];
    const u32 count = typed_options(input, {spans, 25});
    check_options(spans, count, c.focused_slot, true, false);
    u32 failing = no_option;
    for (u32 n = 0; n < count && failing == no_option; ++n)
        if (c.option_states[spans[n].slot].error.bytes)
            failing = spans[n].slot;
    const auto list = command_options();
    char added[1024];
    ProtocolWriter w{{reinterpret_cast<u8*>(added), sizeof(added)}, 0};
    for (u32 slot = 0; slot < list.count; ++slot) {
        bool present = false;
        for (u32 n = 0; n < count && !present; ++n)
            present = spans[n].slot == slot;
        if (present || !c.option_states[slot].error.bytes)
            continue;
        char storage[128];
        const auto name = field_text(*c.selected.index, list.options[slot], "name", {storage, sizeof(storage)});
        if (!protocol_writer_append_byte(&w, ' ') ||
            !protocol_writer_append_bytes(&w, {reinterpret_cast<const u8*>(name.data), name.bytes}) ||
            !protocol_writer_append_byte(&w, ':') ||
            !protocol_writer_append_bytes(&w, {reinterpret_cast<const u8*>(kCommandOptionEnd),
                                               kCommandOptionEndBytes}))
            break;
        if (failing == no_option)
            failing = slot;
    }
    if (failing == no_option)
        return true;
    *blocked = true;
    shake_app();
    auto result = w.used ? message_composer_replace_range(&g_app.message_composer, input.bytes, input.bytes,
                                                          {added, w.used})
                         : MessageComposerResult::unchanged;
    if (result == MessageComposerResult::invalid_input)
        return true;
    const auto now = message_composer_text(g_app.message_composer);
    const u32 typed = typed_options(now, {spans, 25});
    for (u32 n = 0; n < typed; ++n)
        if (spans[n].slot == failing && select_chip(spans[n]) == MessageComposerResult::changed)
            result = MessageComposerResult::changed;
    // The bar shows the selected option's failed check.
    c.revision = 0;
    return result == MessageComposerResult::changed ? composer_apply_result(result, false)
                                                    : refresh_message_composer_ui();
}
// The selected command still leads the composer's draft.
bool command_in_draft() noexcept {
    if (!commands || !commands->selected.command || g_app.message_edit.editor_active)
        return false;
    const auto input = message_composer_text(g_app.message_composer);
    const auto name = commands->selected.command->name;
    return name.bytes < 0xffffu && commands->channel == g_app.message_composer.channel_id &&
           input.bytes >= name.bytes + 1 && input.data[0] == '/' && utf8_equal({input.data + 1, name.bytes}, name) &&
           (input.bytes == name.bytes + 1 || command_space(input.data[name.bytes + 1]));
}

} // namespace commands_detail

bool application_commands_bar_visible() noexcept {
    return command_in_draft();
}
void application_commands_composer(MessageComposerProjectionContext* projection) noexcept {
    reset_value(&projection->command);
    projection->option_chip_count = 0;
    projection->command_bar_name = projection->command_bar_detail = {};
    projection->command_bar_error = false;
    if (!command_in_draft())
        return;
    const auto& c = *commands;
    const auto input = message_composer_text(g_app.message_composer);
    const auto name = c.selected.command->name;
    projection->command.name_bytes = static_cast<u16>(name.bytes + 1);
    // The typed options draw as option chips: the active one
    // (caret inside, editor focused) and failed ones are outlined.
    // A command not set up yet has no option states of its own.
    const bool prepared = c.prepared == c.selected.command;
    if (input.bytes < 0xffffu) {
        if (prepared)
            update_focus(input);
        CommandArgumentSpan spans[25];
        const u32 count = typed_options(input, {spans, 25});
        const u32 active = chip_at(input, spans, count, g_app.message_composer.caret);
        for (u32 n = 0; n < count; ++n) {
            const bool selected = n == active && g_app.message_composer.focused;
            const u8 flags = selected ? message_composer_chip_active
                             : prepared && c.option_states[spans[n].slot].error.bytes ? message_composer_chip_error
                                                                                       : 0;
            const u32 head = spans[n].key_first > 0 && command_space(input.data[spans[n].key_first - 1])
                                 ? spans[n].key_first - 1
                                 : spans[n].key_first;
            projection->option_chips[n] = {static_cast<u16>(head), static_cast<u16>(spans[n].key_first),
                                           static_cast<u16>(spans[n].value_first),
                                           static_cast<u16>(spans[n].value_last),
                                           static_cast<u16>(spans[n].end_last), flags};
        }
        projection->option_chip_count = count;
    }
    // The option bar: the active option's name and
    // description, its failed check or the command's error instead in the
    // danger colour; else the command's "/name" and description, or its error.
    projection->command_bar_name = copy_label(commands->bar_name, sizeof(commands->bar_name), utf8_literal("/"), name);
    projection->command_bar_detail = c.selected.command->description;
    projection->command_bar_error = false;
    if (prepared && c.focused_option.source.bytes) {
        const auto& state = c.option_states[c.focused_slot];
        projection->command_bar_name =
            field_text(*c.selected.index, c.focused_option, "name", {commands->bar_name, sizeof(commands->bar_name)});
        const Utf8View failure = state.error.bytes ? state.error : c.error;
        projection->command_bar_detail =
            failure.bytes ? failure
                          : field_text(*c.selected.index, c.focused_option, "description",
                                       {commands->bar_detail, sizeof(commands->bar_detail)});
        projection->command_bar_error = failure.bytes != 0;
    } else if (c.error.bytes) {
        projection->command_bar_detail = c.error;
        projection->command_bar_error = true;
    }
    if (c.selected.app)
        projection->command.avatar = app_picture(c.selected.app);
}

} // namespace maicord::app_detail
