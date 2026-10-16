#include "maicord/app/command_autocomplete.hpp"
#include "application_commands.hpp"
#include "application_state.hpp"
#include "application_composer.hpp"
#include "application_scrolling.hpp"
#include "application_account_presentation.hpp"
#include "application_message_send.hpp"
#include "application_plugins.hpp"
#include "application_user_actions.hpp"
#include "maicord/protocol/application_commands.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/log.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
constexpr u32 choose_base = 100000, rail_base = 200000, option_base = 300000, value_base = 400000;
// Local plugin commands, ordered as application_plugins_commands returns them.
constexpr u32 plugin_base = 500000;
constexpr u32 plugin_rows = 8;
constexpr float row_height = 56, rail_height = 40;
struct Entry {
    const ApplicationCommand* command;
    const CommandIndex* index;
    const CommandApplication* app;
};
struct Commands {
    CommandIndex index[2];
    Arena projection;
    Entry* entries;
    u32 count;
    const CommandApplication** apps;
    u32 app_count;
    Entry selected;
    CommandAutocomplete autocomplete;
    u64 issued_nonce, nonce_serial;
    bool suggestions_dirty;
    JsonValue focused_option;
    u32 value_begin, value_end, choice_count, accepted_revision;
    bool value_quoted, value_closed, choices_shown;
    char choice_values[25][512];
    u32 choice_bytes[25];
    u64 guild, channel, filter_app, requested_channel, requested_guild, retry_at;
    u32 revision, generation, rendered_generation, submitted_revision;
    u64 submitted_document, dismissed_document;
    u32 dismissed_revision;
    u8 pending, operation, requested_slot;
    bool loaded[2], sending, queued;
    float offset, rail_offset;
    u32 keyboard_row, option_count;
    u8 attempts[2];
    Utf8View error;
    char labels[32][512], details[32][1024], payload[65536];
    u32 payload_bytes;
    u64 nonce, send_application, send_channel, send_guild;
    // A context menu wants the index loaded; a context invocation is queued
    // or in flight (it never touches the composer draft).
    bool context_wanted, context_send;
};
Commands* commands;
Utf8View field_text(const CommandIndex& index, JsonValue value, const char* key,
                    Slice<char> scratch) noexcept {
    u32 n = 0;
    while (key[n])
        ++n;
    Utf8View text{};
    command_json_text(command_member(index.document, value, {key, n}), scratch, &text);
    return text;
}
bool command_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}
Utf8View command_query(Utf8View text) noexcept {
    // Root/group/subcommand names cannot contain ':'. Named arguments belong
    // to the composer, not catalog filtering, and must survive selection.
    u32 end = text.bytes;
    u32 token = 0;
    for (u32 at = 0; at < text.bytes; ++at) {
        if (command_space(text.data[at])) token = at + 1;
        else if (text.data[at] == ':') { end = token; break; }
    }
    while (end && command_space(text.data[end - 1])) --end;
    return {text.data, end};
}
Utf8View copy_label(char* out, u32 capacity, Utf8View a, Utf8View b = {}, Utf8View c = {}) noexcept {
    u32 n = 0;
    const Utf8View parts[] = {a, b, c};
    for (const auto part : parts)
        n += utf8_copy_truncated(out + n, capacity - n, part);
    return {out, n};
}
void close_popup() noexcept {
    if (g_app.popup_owner_kind == PopupOwnerKind::commands) {
        popup_scene_close(&g_app.popup_scene);
        set_popup_owner(PopupOwnerKind::none);
    }
}
const CommandApplication* find_app(const CommandIndex& source, u64 id) noexcept {
    for (u32 n = 0; n < source.application_count; ++n)
        if (source.applications[n].id == id)
            return source.applications + n;
    return nullptr;
}
bool project(Utf8View query) noexcept {
    auto& c = *commands;
    query = command_query(query);
    arena_release(&c.projection);
    c.entries = nullptr;
    c.apps = nullptr;
    c.count = c.app_count = 0;
    const u32 capacity = c.index[0].command_count + c.index[1].command_count;
    const usize bytes =
        maximum<usize>(4096, static_cast<usize>(capacity + 1) * (sizeof(Entry) + sizeof(void*)));
    if (!arena_initialize(&c.projection, bytes, 0, 4096))
        return false;
    c.entries = arena_allocate<Entry>(&c.projection, maximum(1u, capacity));
    c.apps = arena_allocate<const CommandApplication*>(&c.projection, maximum(1u, capacity));
    if (!c.entries || !c.apps)
        return false;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, c.channel);
    for (const auto& source : c.index)
        for (u32 n = 0; n < source.command_count; ++n) {
            const auto& command = source.commands[n];
            if ((!c.guild && !command.dm_permission) || (command.guild_id && command.guild_id != c.guild))
                continue;
            if (channel && channel->permissions_resolved &&
                !(channel->effective_permissions & (1ull << 31)))
                continue;
            bool duplicate = false;
            for (u32 j = 0; j < c.count; ++j)
                if (c.entries[j].command->id == command.id &&
                    utf8_equal(c.entries[j].command->name, command.name)) {
                    duplicate = true;
                    break;
                }
            if (duplicate)
                continue;
            const auto* app = find_app(source, command.application_id);
            if (app) {
                bool seen = false;
                for (u32 j = 0; j < c.app_count; ++j)
                    if (c.apps[j]->id == app->id) {
                        seen = true;
                        break;
                    }
                if (!seen)
                    c.apps[c.app_count++] = app;
            }
            if (c.filter_app && command.application_id != c.filter_app)
                continue;
            if (!ascii_contains_ignore_case(command.name, query) &&
                !(app && ascii_contains_ignore_case(app->name, query)))
                continue;
            c.entries[c.count++] = {&command, &source, app};
        }
    c.offset = c.rail_offset = 0;
    c.keyboard_row = 0;
    ++c.generation;
    return true;
}
void avatar(PopupItem& item, const CommandApplication* app) noexcept {
    if (!app || !app->bot_id)
        return;
    const auto* user = user_store_find(g_app.account_ui->user_store, app->bot_id);
    if (!user)
        return;
    item.flags |= popup_item_avatar;
    item.media_id = user->id;
    const auto hash = user_store_avatar_hash(*user);
    item.media_variant = user_avatar_variant(hash);
    item.media_flags = hash.bytes > 2 && hash.data[0] == 'a' && hash.data[1] == '_' ? 1 : 0;
}
struct OptionsUi {
    Commands* c;
    PopupItem* items;
    u32* count;
};
bool option_row(void* opaque, u32 index, JsonValue option) noexcept {
    auto& ctx = *static_cast<OptionsUi*>(opaque);
    ++ctx.c->option_count;
    const u32 first = static_cast<u32>(ctx.c->offset / row_height);
    if (index + 1 < first || *ctx.count == 23)
        return true;
    const u32 n = (*ctx.count)++;
    auto& c = *ctx.c;
    const auto name = field_text(*c.selected.index, option, "name", {c.labels[n], 512});
    const auto description = field_text(*c.selected.index, option, "description", {c.details[n], 1024});
    ctx.items[n] = {name, option_base + index, popup_item_enabled | popup_item_command_row, 0};
    ctx.items[n].detail = description;
    return true;
}
struct ChoicesUi {
    Commands* c;
    PopupItem* items;
    u32* count;
    Utf8View query;
};
bool choice_row(void* opaque, u32, JsonValue choice) noexcept {
    auto& ui = *static_cast<ChoicesUi*>(opaque);
    auto& c = *ui.c;
    if (c.choice_count == 25)
        return true;
    const u32 n = *ui.count;
    char scratch[512];
    const auto name = field_text(*c.selected.index, choice, "name", {scratch, 512});
    if (!name.bytes || !ascii_contains_ignore_case(name, ui.query))
        return true;
    const auto value = command_member(c.selected.index->document, choice, utf8_literal("value"));
    const u32 slot = c.choice_count;
    Utf8View encoded{};
    if (value.kind == JsonValueKind::string) {
        if (!command_json_text(value, {c.choice_values[slot], 512}, &encoded))
            return true;
    } else
        encoded = copy_label(c.choice_values[slot], 512, value.source);
    c.choice_bytes[slot] = encoded.bytes;
    if (slot + 1 >= static_cast<u32>(c.offset / row_height) && *ui.count < 31)
        ui.items[(*ui.count)++] = {copy_label(c.labels[n], 512, name), value_base + slot,
                                   popup_item_enabled | popup_item_command_row, 0};
    ++c.choice_count;
    return true;
}
bool autocomplete_enabled() noexcept {
    auto& c = *commands;
    bool enabled = false;
    return c.selected.command && c.focused_option.source.bytes &&
           json_value_to_bool(command_member(c.selected.index->document, c.focused_option,
                                             utf8_literal("autocomplete")),
                              &enabled) &&
           enabled;
}
u64 next_nonce() noexcept {
    const u64 stamp = (clock_unix_milliseconds() - 1420070400000ull) << 22;
    auto& serial = commands->nonce_serial;
    serial = maximum(serial + 1, stamp);
    return serial;
}
bool focused_query(Slice<char> scratch, Utf8View* query) noexcept {
    const auto& c = *commands;
    const auto input = message_composer_text(g_app.message_composer);
    if (!c.focused_option.source.bytes || c.value_begin > c.value_end || c.value_end > input.bytes)
        return false;
    CommandArgumentFocus focus{};
    focus.value = {input.data + c.value_begin, c.value_end - c.value_begin};
    focus.quoted = c.value_quoted;
    focus.closed = c.value_closed;
    return command_argument_query(focus, scratch, query);
}
bool render_choices(PopupItem* items, u32* count) noexcept {
    auto& c = *commands;
    c.choice_count = 0;
    char query_storage[16384];
    Utf8View query{};
    if (!focused_query({query_storage, sizeof(query_storage)}, &query))
        return false;
    if (autocomplete_enabled()) {
        const auto& state = c.autocomplete;
        if (state.phase == CommandAutocompletePhase::ready) {
            for (u32 slot = 0; slot < state.suggestions.count; ++slot) {
                const auto& choice = state.suggestions.choices[slot];
                c.choice_bytes[slot] = copy_label(c.choice_values[slot], 512, choice.value).bytes;
                ++c.choice_count;
                if (slot + 1 < static_cast<u32>(c.offset / row_height) || *count >= 31)
                    continue;
                const u32 n = (*count)++;
                items[n] = {copy_label(c.labels[n], 512, choice.name), value_base + slot,
                            popup_item_enabled | popup_item_command_row, 0};
            }
        }
        if (!c.choice_count && *count < 31) {
            const auto label =
                state.phase == CommandAutocompletePhase::failed  ? utf8_literal("Suggestions unavailable")
                : state.phase == CommandAutocompletePhase::ready ? utf8_literal("No suggestions")
                                                                 : utf8_literal("Loading suggestions…");
            items[(*count)++] = {label, value_base - 1, popup_item_command_row, 0};
        }
        return true;
    }
    const auto choices =
        command_member(c.selected.index->document, c.focused_option, utf8_literal("choices"));
    if (choices.source.bytes) {
        ChoicesUi ui{&c, items, count, query};
        json_visit_indexed_array(c.selected.index->document, choices, 64, choice_row, &ui);
    } else {
        u64 type = 0;
        json_value_to_u64(
            command_member(c.selected.index->document, c.focused_option, utf8_literal("type")),
            &type);
        if (type == 5) {
            const Utf8View values[] = {utf8_literal("true"), utf8_literal("false")};
            for (auto value : values)
                if (ascii_contains_ignore_case(value, query)) {
                    const u32 slot = c.choice_count++;
                    c.choice_bytes[slot] = copy_label(c.choice_values[slot], 512, value).bytes;
                    items[(*count)++] = {value, value_base + slot,
                                         popup_item_enabled | popup_item_command_row, 0};
                }
        }
    }
    return c.choice_count != 0;
}
bool render() noexcept {
    auto& c = *commands;
    const auto* surface = ui_widget_manager_layout_node(g_app.message_composer_scene.widgets,
                                                         g_app.message_composer_scene.surface);
    if (!surface)
        return true;
    PopupItem items[32]{};
    u32 count = 0;
    const u32 rail_first = static_cast<u32>(c.rail_offset / rail_height);
    for (u32 r = rail_first; r <= c.app_count && count < 11; ++r) {
        auto* app = r ? c.apps[r - 1] : nullptr;
        items[count] = {app ? app->name : utf8_literal("All"), rail_base + r,
                        popup_item_enabled | popup_item_command_category, 0};
        if ((app ? app->id : 0) == c.filter_app)
            items[count].flags |= popup_item_checked;
        avatar(items[count], app);
        ++count;
    }
    if (!count)
        items[count++] = {utf8_literal("All"), rail_base, popup_item_enabled | popup_item_command_category,
                          0};
    const u32 body_begin = count;
    NativePluginSdkCommandView local[plugin_rows];
    u32 local_count = 0;
    if (!c.selected.command && c.offset < row_height) {
        const auto input = message_composer_text(g_app.message_composer);
        local_count = input.bytes ? application_plugins_commands(command_query({input.data + 1, input.bytes - 1}),
                                                                 {local, plugin_rows}) : 0;
        for (u32 n = 0; n < local_count && count < 23; ++n) {
            items[count] = {copy_label(c.labels[count], 512, utf8_literal("/"), local[n].name), plugin_base + n,
                            popup_item_enabled | popup_item_command_row, 0};
            items[count].detail = copy_label(c.details[count], 1024, local[n].description,
                                             local[n].description.bytes ? utf8_literal(" \xC2\xB7 ") : Utf8View{},
                                             utf8_literal("Plugin"));
            ++count;
        }
    }
    if (c.error.bytes || (!c.selected.command && !c.count && !local_count)) {
        items[count++] = {c.error.bytes ? c.error
                          : c.pending   ? utf8_literal("Loading commands…")
                                        : utf8_literal("No matching commands"),
                          choose_base - 1, popup_item_command_row, 0};
    } else if (c.selected.command) {
        if (c.offset < row_height) {
            items[count] = {copy_label(c.labels[count], 512, utf8_literal("/"), c.selected.command->name),
                            choose_base - 2, popup_item_command_row, 0};
            items[count++].detail = c.sending || c.queued
                                        ? utf8_literal("Sending…")
                                        : utf8_literal("Tab to edit options · Enter to run");
        }
        c.option_count = 0;
        c.choices_shown = render_choices(items, &count);
        if (c.choices_shown)
            c.option_count = c.choice_count;
        else if (c.selected.command->options.source.bytes) {
            OptionsUi options{&c, items, &count};
            json_visit_indexed_array(c.selected.index->document, c.selected.command->options, 64,
                                     option_row, &options);
        }
    } else {
        const u32 first = static_cast<u32>(c.offset / row_height);
        for (u32 n = first; n < c.count && count < 23; ++n) {
            const auto& entry = c.entries[n];
            auto& item = items[count];
            item = {copy_label(c.labels[count], 512, utf8_literal("/"), entry.command->name),
                    choose_base + n, popup_item_enabled | popup_item_command_row, 0};
            item.detail = copy_label(c.details[count], 1024, entry.command->description,
                                     entry.app ? utf8_literal(" · ") : Utf8View{},
                                     entry.app ? entry.app->name : Utf8View{});
            avatar(item, entry.app);
            ++count;
        }
    }
    const UiRect viewport = application_viewport_dips();
    const UiRect anchor = surface->rectangle;
    const auto result =
        popup_scene_open(&g_app.popup_scene, PopupKind::autocomplete, c.channel, {items, count}, anchor,
                         viewport, ui_rect_width(anchor), true,
                         c.offset - static_cast<u32>(c.offset / row_height) * row_height,
                         c.rail_offset - rail_first * rail_height);
    if (!result.ok())
        return false;
    set_popup_owner(PopupOwnerKind::commands);
    g_app.popup_scene.selected_index = ~0u;
    u32 selected_action = 0;
    if (!c.selected.command && c.count) {
        c.keyboard_row = minimum(c.keyboard_row, c.count - 1);
        selected_action = choose_base + c.keyboard_row;
    } else if (c.selected.command && c.option_count) {
        c.keyboard_row = minimum(maximum(c.keyboard_row, 1u), c.option_count);
        selected_action = (c.choices_shown ? value_base : option_base) + c.keyboard_row - 1;
    }
    for (u32 n = body_begin; n < count; ++n)
        if (items[n].action == selected_action && (items[n].flags & popup_item_enabled)) {
            g_app.popup_scene.selected_index = n;
            break;
        }
    c.rendered_generation = c.generation;
    return true;
}
} // namespace
bool application_commands_sync(bool* handled) noexcept {
    *handled = false;
    const auto input = message_composer_text(g_app.message_composer);
    if (!g_app.account_ui || !input.bytes || input.data[0] != '/' || g_app.message_edit.editor_active) {
        close_popup();
        if (commands) {
            commands->selected = {};
            commands->revision = 0;
        }
        return true;
    }
    *handled = true;
    if (!commands)
        commands = static_cast<Commands*>(
            VirtualAlloc(nullptr, sizeof(Commands), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!commands)
        return false;
    auto& c = *commands;
    const auto& composer = g_app.message_composer;
    const bool submitted_draft = c.submitted_document == composer.document_identity &&
                                 c.submitted_revision == composer.text_revision;
    if (((c.queued || c.sending) && submitted_draft) ||
        (c.dismissed_document == composer.document_identity && c.dismissed_revision == composer.text_revision)) {
        close_popup();
        return true;
    }
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                           g_app.message_composer.channel_id);
    const u64 guild = channel ? channel->guild_id : 0;
    if (c.channel != g_app.message_composer.channel_id) {
        if (c.guild != guild || !guild) {
            c.loaded[0] = false;
            c.pending |= 1;
            command_index_release(&c.index[0]);
            c.attempts[0] = 0;
        }
        c.channel = g_app.message_composer.channel_id;
        c.guild = guild;
        c.queued = false;
        c.selected = {};
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
            (input.bytes > name.bytes + 1 && !command_space(input.data[name.bytes + 1])))
            c.selected = {};
    }
    const bool changed = c.revision != g_app.message_composer.revision;
    if (!c.selected.command)
        c.focused_option = {};
    if (changed) {
        c.focused_option = {};
        const u32 arguments_start = c.selected.command ? c.selected.command->name.bytes + 1 : 0;
        CommandArgumentFocus focus{};
        if (c.selected.command && c.accepted_revision != g_app.message_composer.revision &&
            arguments_start <= input.bytes && g_app.message_composer.caret >= arguments_start &&
            command_argument_focus(*c.selected.index, *c.selected.command,
                                   {input.data + arguments_start, input.bytes - arguments_start},
                                   g_app.message_composer.caret - arguments_start, &focus)) {
            c.focused_option = focus.option;
            c.value_begin = arguments_start + focus.value_begin;
            c.value_end = arguments_start + focus.value_end;
            c.value_quoted = focus.quoted;
            c.value_closed = focus.closed;
        }
    }
    if (changed) {
        c.revision = g_app.message_composer.revision;
        ++c.generation;
        c.keyboard_row = c.selected.command ? 1 : 0;
        c.error = {};
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
    if (action >= plugin_base) {
        const auto input = message_composer_text(g_app.message_composer);
        NativePluginSdkCommandView local[plugin_rows];
        const u32 local_count = input.bytes ? application_plugins_commands(
            command_query({input.data + 1, input.bytes - 1}), {local, plugin_rows}) : 0;
        if (action - plugin_base >= local_count)
            return true;
        char insertion[64];
        const auto value = copy_label(insertion, 64, utf8_literal("/"), local[action - plugin_base].name, utf8_literal(" "));
        edit_result = message_composer_replace_range(&g_app.message_composer, 0, input.bytes, value);
        c.revision = 0;
    } else if (action >= value_base) {
        const u32 slot = action - value_base;
        if (!c.selected.command || !c.selected.index || !c.focused_option.source.bytes ||
            !c.choices_shown || slot >= c.choice_count)
            return true;
        char insertion[1028];
        ProtocolWriter writer{{reinterpret_cast<u8*>(insertion), sizeof(insertion)}, 0};
        u64 type = 0;
        json_value_to_u64(
            command_member(c.selected.index->document, c.focused_option, utf8_literal("type")),
            &type);
        const Utf8View chosen{c.choice_values[slot], c.choice_bytes[slot]};
        bool ok = type == 3 ? protocol_writer_append_json_string(&writer, chosen)
                            : protocol_writer_append_bytes(
                                  &writer, {reinterpret_cast<const u8*>(chosen.data), chosen.bytes});
        const auto input = message_composer_text(g_app.message_composer);
        if (c.value_end == input.bytes)
            ok = ok && protocol_writer_append_byte(&writer, ' ');
        if (ok && c.value_begin <= c.value_end && c.value_end <= input.bytes)
            edit_result = message_composer_replace_range(&g_app.message_composer, c.value_begin,
                                                         c.value_end, {insertion, writer.used});
        c.accepted_revision = g_app.message_composer.revision;
        c.focused_option = {};
        c.choice_count = 0;
        c.offset = 0;
    } else if (action >= option_base) {
        const u32 slot = action - option_base;
        if (!c.selected.command || !c.selected.index || c.choices_shown || slot >= c.option_count)
            return true;
        struct Pick {
            u32 slot;
            JsonValue value;
        } pick{slot};
        json_visit_indexed_array(
            c.selected.index->document, c.selected.command->options, 64,
            [](void* p, u32 n, JsonValue v) noexcept {
                auto& pick = *static_cast<Pick*>(p);
                if (n == pick.slot)
                    pick.value = v;
                return true;
            },
            &pick);
        char name[128];
        const auto key = field_text(*c.selected.index, pick.value, "name", {name, 128});
        const auto input = message_composer_text(g_app.message_composer);
        const u32 start = c.selected.command->name.bytes + 1;
        CommandArgumentFocus existing{};
        c.accepted_revision = 0;
        c.revision = 0;
        if (start <= input.bytes &&
            command_argument_find(*c.selected.index, *c.selected.command,
                                  {input.data + start, input.bytes - start}, key, &existing)) {
            edit_result = message_composer_set_selection(
                &g_app.message_composer, start + existing.value_begin, start + existing.value_end);
        } else {
            char insertion[140];
            const bool separated =
                input.bytes && (input.data[input.bytes - 1] == ' ' || input.data[input.bytes - 1] == '\t' ||
                                input.data[input.bytes - 1] == '\n');
            const auto value = copy_label(insertion, 140, separated ? Utf8View{} : utf8_literal(" "), key,
                                          utf8_literal(":"));
            edit_result =
                message_composer_replace_range(&g_app.message_composer, input.bytes, input.bytes, value);
        }
        c.offset = 0;
    } else if (action >= rail_base) {
        const u32 n = action - rail_base;
        if (n > c.app_count)
            return true;
        c.filter_app = n ? c.apps[n - 1]->id : 0;
        c.selected = {};
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
            command_space(input.data[prefix]);
        edit_result = message_composer_replace_range(&g_app.message_composer, 0,
            named_arguments ? prefix + 1 : input.bytes, value);
    }
    return edit_result == MessageComposerResult::changed
               ? composer_apply_result(edit_result, text_revision != g_app.message_composer.text_revision)
               : refresh_message_composer_ui();
}
bool application_commands_send(bool* handled) noexcept {
    const auto input = message_composer_text(g_app.message_composer);
    *handled = input.bytes && input.data[0] == '/';
    if (!*handled)
        return true;
    if (g_app.account.status.required_action != AccountRequirement::none) return true;
    if (!commands || !commands->selected.command) {
        bool ignored;
        return application_commands_sync(&ignored);
    }
    auto& c = *commands;
    if (c.sending || c.queued)
        return true;
    char options[16384];
    ProtocolWriter writer{{reinterpret_cast<u8*>(options), sizeof(options)}, 0};
    const u32 start = c.selected.command->name.bytes + 1;
    if (input.bytes < start ||
        !command_arguments(*c.selected.index, *c.selected.command,
                           {input.data + start, input.bytes - start}, &writer, &c.error))
        return render();
    ProtocolWriter data{{reinterpret_cast<u8*>(c.payload), sizeof(c.payload)}, 0};
    if (!command_data(*c.selected.command, {options, writer.used}, &data)) {
        c.error = utf8_literal("Command is too large to send");
        return render();
    }
    c.payload_bytes = data.used;
    c.queued = true;
    c.submitted_revision = g_app.message_composer.text_revision;
    c.submitted_document = g_app.message_composer.document_identity;
    c.send_application = c.selected.command->application_id;
    c.send_channel = c.channel;
    c.send_guild = c.guild;
    c.nonce = next_nonce();
    command_autocomplete_cancel(&c.autocomplete);
    c.error = {};
    close_popup();
    return refresh_message_composer_ui();
}
bool application_commands_pending(u64 now) noexcept {
    if (!commands || !g_app.account_ui || !g_app.account.api.application_command) return false;
    const auto& c = *commands;
    const bool suggest = c.autocomplete.phase == CommandAutocompletePhase::debounce && now >= c.autocomplete.due_at;
    return !c.sending && c.retry_at <= now && (c.pending || c.queued || suggest) &&
           (c.queued || c.context_wanted || g_app.popup_owner_kind == PopupOwnerKind::commands);
}
bool application_commands_submit(u64 now) noexcept {
    if (!application_commands_pending(now)) return false;
    auto& c = *commands;
    const u8 slot = (c.pending & 1) ? 0 : 1;
    const u8 op = c.queued ? 2 : c.pending ? slot : 3;
    u64 nonce = op == 2 ? c.nonce : op == 3 ? next_nonce() : 0;
    u64 application = op == 2 ? c.send_application : 0;
    Utf8View payload = op == 2 ? Utf8View{c.payload, c.payload_bytes} : Utf8View{};
    if (op == 3) {
        char name_storage[128], options[16384], query_storage[16384];
        Utf8View query{};
        const auto name = field_text(*c.selected.index, c.focused_option, "name", {name_storage, 128});
        const auto input = message_composer_text(g_app.message_composer);
        const u32 start = c.selected.command->name.bytes + 1;

        ProtocolWriter w{{reinterpret_cast<u8*>(options), sizeof(options)}, 0};
        Utf8View error{};
        ProtocolWriter data{{reinterpret_cast<u8*>(c.payload), sizeof(c.payload)}, 0};
        if (start > input.bytes || !focused_query({query_storage, sizeof(query_storage)}, &query) ||
            !command_arguments(*c.selected.index, *c.selected.command,
                               {input.data + start, input.bytes - start}, &w, &error, name,
                               query) ||
            !command_data(*c.selected.command, {options, w.used}, &data)) {
            command_autocomplete_begin(&c.autocomplete, nonce, now);
            command_autocomplete_transport(&c.autocomplete, nonce, false);
            c.suggestions_dirty = true;
            return true;
        }
        application = c.selected.command->application_id;
        payload = {c.payload, data.used};
    }
    auto& ui = *g_app.account_ui;
    if (++ui.message_history_request_id == 0)
        ++ui.message_history_request_id;
    const auto result = g_app.account.api.application_command(
        g_app.account.instance, op == 2 ? c.send_channel : c.channel, op == 2 ? c.send_guild : c.guild,
        application, nonce, payload, ui.message_history_request_id, op);
    if (result.error == RestClientSubmitError::busy) {
        c.retry_at = now + 100;
        return true;
    }
    if (op == 3)
        command_autocomplete_begin(&c.autocomplete, nonce, now);
    if (!result.ok()) {
        if (op == 3) {
            command_autocomplete_transport(&c.autocomplete, nonce, false);
            c.suggestions_dirty = true;
        } else {
            c.error = utf8_literal("Couldn't load commands. Try again.");
            c.queued = false;
            c.dismissed_document = 0;
            c.revision = g_app.message_composer.revision;
            if (op < 2) {
                c.pending = 0;
                c.loaded[0] = c.loaded[1] = true;
            }
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
    c.retry_at = 0;
    if (op == 2) {
        c.queued = false;
        c.sending = true;
    } else if (op < 2)
        c.pending &= ~(1u << slot);
    g_app.details_request_owner = MessageHistoryRequestOwner::commands;
    g_app.details_request_id = ui.message_history_request_id;
    return true;
}
bool application_commands_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!commands)
        return true;
    auto& c = *commands;
    const bool same = c.requested_channel == c.channel && c.requested_guild == c.guild;
    if (c.operation == 3) {
        c.suggestions_dirty =
            command_autocomplete_transport(&c.autocomplete, c.issued_nonce,
                                           response.transport_ok() && response.status_code >= 200 &&
                                               response.status_code < 300) ||
            c.suggestions_dirty;
        return application_commands_tick(clock_monotonic_milliseconds());
    }
    if (c.operation == 2 && c.context_send) {
        c.sending = false;
        c.context_send = false;
        if (!response.transport_ok() || response.status_code < 200 || response.status_code >= 300)
            log_u64(LogLevel::warning, "commands", "context_command_status", response.status_code);
        return true;
    }
    if (c.operation == 2) {
        c.sending = false;
        if (response.transport_ok() && response.status_code >= 200 && response.status_code < 300) {
            if (c.send_channel == g_app.message_composer.channel_id &&
                c.submitted_document == g_app.message_composer.document_identity &&
                c.submitted_revision == g_app.message_composer.text_revision) {
                const auto cleared = message_composer_clear(&g_app.message_composer);
                c.selected = {};
                close_popup();
                ++c.generation;
                return composer_apply_result(cleared, true);
            }
        } else if (same) {
            c.dismissed_document = 0;
            c.revision = g_app.message_composer.revision;
            c.error = utf8_literal("Command failed. Try again.");
        }
    } else if (same || c.requested_slot == 1) {
        if (response.transport_ok() &&
            ((response.status_code == 202 && c.attempts[c.requested_slot]++ < 3) ||
             (response.status_code == 429 && response.rate_limited && response.retry_at_ms &&
              !api_rejection_requires_action(response.rejection)))) {
            c.pending |= 1u << c.requested_slot;
            c.retry_at = response.status_code == 429 ? response.retry_at_ms :
                deadline_after(clock_monotonic_milliseconds(), 5000);
        } else {
            c.pending &= ~(1u << c.requested_slot);
            c.loaded[c.requested_slot] = true;
            if (response.transport_ok() && response.status_code == 200) {
                if (c.selected.index == &c.index[c.requested_slot])
                    c.selected = {};
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
            if (c.context_wanted && !c.pending)
                c.context_wanted = false;
            application_user_actions_commands_changed();
        }
    }
    ++c.generation;
    return refresh_message_composer_ui();
}
bool application_commands_scroll_range(bool rail, UiScrollRange* range) noexcept {
    if (!commands || g_app.popup_owner_kind != PopupOwnerKind::commands || !g_app.popup_scene.active)
        return false;
    auto rect = g_app.popup_scene.rectangle;
    rect.left += 6;
    rect.right -= 6;
    rect.top += 6;
    rect.bottom -= 6;
    if (rail)
        rect.right = minimum(rect.right, rect.left + 48);
    else
        rect.left += 48;
    const auto& c = *commands;
    const double extent = rail ? rail_height : row_height;
    const double height =
        rail ? (c.app_count + 1) * extent : (c.selected.command ? c.option_count + 1 : c.count) * extent;
    *range = {rect,
              rail ? c.rail_offset : c.offset,
              maximum(0.0, height - ui_rect_height(rect)),
              extent,
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
bool application_commands_key(u32 key) noexcept {
    if (!commands)
        return true;
    auto& c = *commands;
    const u32 first = c.selected.command ? 1u : 0u;
    const u32 count = c.selected.command ? c.option_count : c.count;
    const u32 action_base = !c.selected.command ? choose_base : c.choices_shown ? value_base : option_base;
    const u32 hovered_action = popup_scene_selected_action(g_app.popup_scene);
    if (hovered_action >= action_base && hovered_action - action_base < count)
        c.keyboard_row = first + hovered_action - action_base;
    if (key == VK_RETURN && !c.selected.command && application_plugins_command_pending()) {
        close_popup();
        return queue_message_send();
    }
    if (key == VK_RETURN || key == VK_TAB) {
        if (key == VK_RETURN && c.selected.command && !c.choices_shown) {
            bool handled;
            return application_commands_send(&handled);
        }
        if (count && c.keyboard_row >= first && c.keyboard_row - first < count)
            return application_commands_activate(action_base + c.keyboard_row - first);
        return true;
    }
    if (key == VK_UP || key == VK_DOWN) {
        if (!count)
            return true;
        c.keyboard_row = minimum(maximum(c.keyboard_row, first), first + count - 1);
        if (key == VK_UP) {
            if (c.keyboard_row > first)
                --c.keyboard_row;
        } else if (c.keyboard_row < first + count - 1)
            ++c.keyboard_row;
        const float top = c.keyboard_row * row_height;
        return reveal_command_row(top, top + row_height) && render();
    }
    return false;
}
void application_commands_dismiss() noexcept {
    if (commands) {
        command_autocomplete_cancel(&commands->autocomplete);
        commands->dismissed_document = g_app.message_composer.document_identity;
        commands->dismissed_revision = g_app.message_composer.text_revision;
    }
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
bool application_commands_tick(u64 now) noexcept {
    if (!commands)
        return true;
    auto& c = *commands;
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
    const bool idle = g_app.details_request_owner == MessageHistoryRequestOwner::none;
    u64 wake = c.autocomplete.phase == CommandAutocompletePhase::waiting ? c.autocomplete.deadline : 0;
    if (!idle)
        return wake;
    if (c.suggestions_dirty)
        return 1;
    if (c.autocomplete.phase == CommandAutocompletePhase::debounce)
        wake = earliest_deadline(wake, maximum(c.autocomplete.due_at, c.retry_at));
    if (c.sending || (!c.queued && (!c.pending || g_app.popup_owner_kind != PopupOwnerKind::commands)))
        return wake;
    return earliest_deadline(wake, c.retry_at ? c.retry_at : 1);
}
namespace {
Commands* ensure_commands() noexcept {
    if (!commands)
        commands = static_cast<Commands*>(
            VirtualAlloc(nullptr, sizeof(Commands), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return commands;
}
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
        if (c.guild != guild || !guild) {
            c.loaded[0] = false;
            if (c.selected.index == &c.index[0])
                c.selected = {};
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
    for (const auto& source : c.index)
        for (u32 n = 0; n < source.context_command_count && !found; ++n)
            if (source.context_commands[n].id == command_id && source.context_types[n] == type)
                found = &source.context_commands[n];
    if (!found)
        return false;
    ProtocolWriter data{{reinterpret_cast<u8*>(c.payload), sizeof(c.payload)}, 0};
    if (!command_context_data(*found, type, target_id, &data))
        return false;
    c.payload_bytes = data.used;
    c.queued = true;
    c.context_send = true;
    // Never matches the composer, so a successful run leaves the draft alone.
    c.submitted_revision = 0;
    c.submitted_document = 0;
    c.send_application = found->application_id;
    c.send_channel = c.channel;
    c.send_guild = c.guild;
    c.nonce = next_nonce();
    if (g_app.window)
        PostMessageW(g_app.window, WM_NULL, 0, 0);
    return true;
}
void application_commands_release() noexcept {
    if (!commands)
        return;
    command_autocomplete_cancel(&commands->autocomplete);
    command_index_release(&commands->index[0]);
    command_index_release(&commands->index[1]);
    arena_release(&commands->projection);
    VirtualFree(commands, 0, MEM_RELEASE);
    commands = nullptr;
}
} // namespace maicord::app_detail
