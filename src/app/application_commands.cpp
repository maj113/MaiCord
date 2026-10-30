#include "maicord/app/command_autocomplete.hpp"
#include "maicord/core/memory.hpp"
#include "application_commands.hpp"
#include "application_state.hpp"
#include "application_composer.hpp"
#include "application_scrolling.hpp"
#include "application_account_presentation.hpp"
#include "application_message_components.hpp"
#include "application_message_send.hpp"
#include "application_text_form.hpp"
#include "application_attachments.hpp"
#include "maicord/protocol/attachment_upload.hpp"
#include "application_plugins.hpp"
#include "application_user_actions.hpp"
#include "maicord/protocol/application_commands.hpp"
#include "maicord/protocol/message_interaction.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/log.hpp"
#include "application_account_access.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
namespace maicord::app_detail {
namespace {
constexpr u32 choose_base = 100000, rail_base = 200000, option_base = 300000, value_base = 400000;
// Local plugin commands, ordered as application_plugins_commands returns them.
constexpr u32 plugin_base = 500000;
constexpr u32 clear_selection = choose_base - 3;
// The rail's Frequently Used entry filters to those commands.
constexpr u64 frequent_filter = ~0ull;
constexpr u32 plugin_rows = 8;
constexpr float row_height = 56, rail_height = 40;
// The official client's local command message ("<user> used <command>") while
// the interaction is sent, created, and when it fails.
constexpr auto echo_sending = utf8_literal("Sending command...");
constexpr auto echo_no_response = utf8_literal("The application did not respond");
constexpr auto echo_failed = utf8_literal("This interaction failed");
// A failure reported this long after sending reads as no response.
constexpr u64 echo_response_window_ms = 3000;
constexpr u16 message_type_chat_command = 20, message_type_context_command = 23;
constexpr u32 echo_slots = 4;
struct Entry {
    const ApplicationCommand* command;
    const CommandIndex* index;
    const CommandApplication* app;
};
enum class EchoPhase : u8 { free, sending, created, settled };
// One local command message on the active timeline; its id is the nonce. The
// sent command data is kept so a failed one can be sent again.
struct Echo {
    u64 nonce, channel, guild, application, author, interaction_id;
    EchoPhase phase;
    bool failed;
    u16 type;
    u8 name_bytes;
    char name[128];
    u32 data_bytes;
    char data[65536];
};
struct Commands {
    CommandIndex index[2];
    Arena projection;
    Arena retired_index_storage;
    Entry* entries;
    u32 count;
    const CommandApplication** apps;
    // Rail apps, and Frequently Used commands of the unfiltered empty query.
    u32 app_count, frequent_count;
    Entry selected;
    CommandAutocomplete autocomplete;
    u64 issued_nonce, nonce_serial;
    bool suggestions_dirty, echo_dirty;
    JsonValue focused_option;
    u32 value_begin, value_end, choice_count, accepted_revision;
    bool value_quoted, value_closed, choices_shown;
    char choice_values[25][512];
    u32 choice_bytes[25];
    u64 guild, channel, filter_app, requested_channel, requested_guild, retry_at;
    u32 revision, generation, rendered_generation;
    u32 channel_index_epoch, requested_index_epoch;
    u64 dismissed_document;
    u32 dismissed_revision, catalog_revision;
    // A command chosen outside the picker (profile Commands, command mention);
    // selected once its index is loaded while the draft starts with its name.
    u64 insert_application, insert_command;
    char insert_name[128];
    u32 insert_name_bytes;
    bool insert_wanted;
    u8 pending, operation, requested_slot;
    bool loaded[2], sending, queued, catalog_dirty, indexing_retry;
    float offset, rail_offset;
    u32 keyboard_row, option_count;
    u8 attempts[2];
    Utf8View error;
    char labels[32][512], details[32][1024], payload[65536];
    u32 payload_bytes;
    u64 nonce, send_application, send_channel, send_guild;
    // A context menu wants the index loaded.
    bool context_wanted;
    Echo echoes[echo_slots];
    u32 echo_next;
};
Commands* commands;
Commands* ensure_commands() noexcept {
    if (!commands)
        commands = static_cast<Commands*>(
            VirtualAlloc(nullptr, sizeof(Commands), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return commands;
}
// Command frecency (settings-proto/2), as the official client keeps it: uses
// are tracked locally, saved a few seconds later and replayed over a fresh
// copy until a save confirms them. Allocated with the first picker or use.
constexpr u32 frecency_pending_uses = 16, frequently_shown = 5, frequently_ranked = 100;
constexpr u64 frecency_save_delay_ms = 5000, frecency_retry_ms = 30000;
struct FrecencyUse {
    u64 at;
    u8 bytes;
    char key[kCommandFrecencyKeyBytes];
};
struct Frecency {
    CommandFrecencyEntry entries[kCommandFrecencyMaximumEntries];
    FrecencyUse pending[frecency_pending_uses];
    u32 count, data_version, pending_count, sent_pending;
    u64 revision, save_due, retry_after;
    bool loaded, fetching, saving;
    u8 scratch[64 * 1024];
    u8 json[96 * 1024];
};
Frecency* frecency;
Frecency* ensure_frecency() noexcept {
    if (!frecency)
        frecency = static_cast<Frecency*>(
            VirtualAlloc(nullptr, sizeof(Frecency), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return frecency;
}
bool frecency_current() noexcept {
    return frecency && frecency->loaded && frecency->revision == g_app.account.status.frecency_settings_revision;
}
// The client's key: id, "\0group", "\0sub", and ":<guild>" for a server's own
// command used in that server.
Utf8View frecency_key(const ApplicationCommand& command, u64 guild, Slice<char> output) noexcept {
    ProtocolWriter w{{reinterpret_cast<u8*>(output.data), output.count}, 0};
    bool ok = protocol_writer_append_u64(&w, command.id);
    if (command.group_name.bytes)
        ok = ok && protocol_writer_append_byte(&w, 0) &&
             protocol_writer_append_bytes(&w, {reinterpret_cast<const u8*>(command.group_name.data),
                                               command.group_name.bytes});
    if (command.sub_name.bytes)
        ok = ok && protocol_writer_append_byte(&w, 0) &&
             protocol_writer_append_bytes(&w, {reinterpret_cast<const u8*>(command.sub_name.data),
                                               command.sub_name.bytes});
    if (command.guild_id && guild)
        ok = ok && protocol_writer_append_byte(&w, ':') && protocol_writer_append_u64(&w, guild);
    return ok && w.used <= kCommandFrecencyKeyBytes ? Utf8View{output.data, w.used} : Utf8View{};
}
// Records a used command (APPLICATION_COMMAND_USED) and schedules the save.
void frecency_track(const ApplicationCommand& command, u64 guild) noexcept {
    char storage[kCommandFrecencyKeyBytes + 32];
    const Utf8View key = frecency_key(command, guild, {storage, sizeof(storage)});
    auto* f = ensure_frecency();
    if (!f || !key.bytes)
        return;
    const u64 now = clock_unix_milliseconds();
    if (f->pending_count == frecency_pending_uses) {
        copy_bytes(f->pending, f->pending + 1, (frecency_pending_uses - 1) * sizeof(FrecencyUse));
        --f->pending_count;
        f->sent_pending = f->sent_pending ? f->sent_pending - 1 : 0;
    }
    auto& use = f->pending[f->pending_count++];
    use.at = now;
    use.bytes = static_cast<u8>(key.bytes);
    copy_bytes(use.key, key.data, key.bytes);
    if (f->loaded) {
        command_frecency_track({f->entries, kCommandFrecencyMaximumEntries}, &f->count, key, now);
        f->count = command_frecency_compute({f->entries, kCommandFrecencyMaximumEntries}, f->count, now);
    }
    if (!f->save_due)
        f->save_due = clock_monotonic_milliseconds() + frecency_save_delay_ms;
}
// Installs a fetched or saved copy and replays uses it does not hold yet.
bool frecency_install(Utf8View body, u32 confirmed) noexcept {
    auto& f = *frecency;
    u32 count = 0, version = 0;
    if (!command_frecency_decode_response(body, {f.scratch, sizeof(f.scratch)},
                                          {f.entries, kCommandFrecencyMaximumEntries}, &count, &version))
        return false;
    f.count = count;
    f.data_version = version;
    f.loaded = true;
    copy_bytes(f.pending, f.pending + confirmed, (f.pending_count - confirmed) * sizeof(FrecencyUse));
    f.pending_count -= confirmed;
    for (u32 n = 0; n < f.pending_count; ++n)
        command_frecency_track({f.entries, kCommandFrecencyMaximumEntries}, &f.count,
                               {f.pending[n].key, f.pending[n].bytes}, f.pending[n].at);
    f.count = command_frecency_compute({f.entries, kCommandFrecencyMaximumEntries}, f.count,
                                       clock_unix_milliseconds());
    return true;
}
// Settings travel only while the account session runs, as for emoji frecency.
bool frecency_ready() noexcept {
    const auto* f = frecency;
    return f && !f->fetching && !f->saving && g_app.account.status.running && !g_app.account.status.failed &&
           g_app.account.status.required_action == AccountRequirement::none;
}
bool frecency_fetch_due(u64 now) noexcept {
    const auto* f = frecency;
    return frecency_ready() && now >= f->retry_after && !frecency_current() &&
           (g_app.popup_owner_kind == PopupOwnerKind::commands || f->pending_count);
}
bool frecency_save_due(u64 now) noexcept {
    const auto* f = frecency;
    return frecency_ready() && now >= f->retry_after && frecency_current() && f->pending_count && f->save_due &&
           now >= f->save_due;
}
// Score (hundredths) of a command among the 100 most frecent entries, or 0.
i32 frequently_score(const ApplicationCommand& command, u64 guild) noexcept {
    if (!frecency_current())
        return 0;
    char storage[kCommandFrecencyKeyBytes + 32];
    const Utf8View key = frecency_key(command, guild, {storage, sizeof(storage)});
    const auto& f = *frecency;
    for (u32 n = 0; n < f.count; ++n) {
        const auto& entry = f.entries[n];
        if (entry.key_bytes != key.bytes || !bytes_equal(entry.key, key.data, key.bytes))
            continue;
        u32 ahead = 0;
        for (u32 other = 0; other < f.count && ahead < frequently_ranked; ++other)
            ahead += f.entries[other].frecency > entry.frecency ? 1u : 0u;
        return ahead < frequently_ranked ? maximum(1, entry.score) : 0;
    }
    return 0;
}
void advance_channel_index_epoch(Commands& c) noexcept {
    if (++c.channel_index_epoch == 0)
        ++c.channel_index_epoch;
}
bool retired_text(Utf8View text, const Arena& storage) noexcept {
    const usize pointer = reinterpret_cast<usize>(text.data);
    const usize base = reinterpret_cast<usize>(storage.base);
    return text.bytes && storage.base && pointer >= base && pointer - base < storage.reserved;
}
Utf8View field_text(const CommandIndex& index, JsonValue value, const char* key,
                    Slice<char> scratch) noexcept {
    Utf8View text{};
    command_json_text(command_member(index.document, value, {key, c_string_length(key)}), scratch, &text);
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
    c.count = c.app_count = c.frequent_count = 0;
    const u32 capacity = c.index[0].command_count + c.index[1].command_count;
    const usize bytes = maximum<usize>(
        4096, static_cast<usize>(capacity + 1 + frequently_shown) * (sizeof(Entry) + sizeof(void*)) + 64);
    if (!arena_initialize(&c.projection, bytes, 0, 4096))
        return false;
    c.entries = arena_allocate<Entry>(&c.projection, capacity + frequently_shown);
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
            if (!ascii_contains_ignore_case(command.name, query) &&
                !(app && ascii_contains_ignore_case(app->name, query)))
                continue;
            c.entries[c.count++] = {&command, &source, app};
        }
    // Without a query, the official client leads with up to five Frequently
    // Used commands (by frecency score); they also stay in their app's list,
    // and the rail's clock entry shows them alone.
    u32 picked[frequently_shown]{};
    i32 scores[frequently_shown]{};
    u32 frequent = 0;
    for (u32 n = 0; !query.bytes && n < c.count; ++n) {
        const i32 score = frequently_score(*c.entries[n].command, c.guild);
        if (!score)
            continue;
        u32 at = frequent < frequently_shown ? frequent++ : frequently_shown;
        while (at && scores[at - 1] < score) {
            if (at < frequently_shown) {
                picked[at] = picked[at - 1];
                scores[at] = scores[at - 1];
            }
            --at;
        }
        if (at < frequently_shown) {
            picked[at] = n;
            scores[at] = score;
        }
    }
    c.frequent_count = frequent;
    if (c.filter_app == frequent_filter && !query.bytes) {
        Entry chosen[frequently_shown];
        for (u32 n = 0; n < frequent; ++n)
            chosen[n] = c.entries[picked[n]];
        copy_bytes(c.entries, chosen, frequent * sizeof(Entry));
        c.count = frequent;
    } else if (c.filter_app && c.filter_app != frequent_filter) {
        u32 kept = 0;
        for (u32 n = 0; n < c.count; ++n)
            if (c.entries[n].command->application_id == c.filter_app)
                c.entries[kept++] = c.entries[n];
        c.count = kept;
    } else if (frequent) {
        for (u32 n = c.count; n-- > 0;)
            c.entries[n + frequent] = c.entries[n];
        for (u32 n = 0; n < frequent; ++n)
            c.entries[n] = c.entries[picked[n] + frequent];
        c.count += frequent;
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
    if (*ctx.count == 31)
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
float option_row_height() noexcept {
    const auto* styles = g_app.popup_scene.theme_styles;
    return styles && styles->ready ? styles->metrics.menu_detail_height : row_height;
}
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
    // The draft shows the choice name; execution sends its value (command_arguments).
    const u32 slot = c.choice_count;
    c.choice_bytes[slot] = copy_label(c.choice_values[slot], 512, name).bytes;
    if (*ui.count < 31)
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
    const u64 stamp = (clock_unix_milliseconds() - kSnowflakeEpochMilliseconds) << 22;
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
                if (*count >= 31)
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
            const Utf8View values[] = {utf8_literal("True"), utf8_literal("False")};
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
// The client's usage summary after a command name: its required option names,
// then "+N optional" for the rest.
struct OptionSummary {
    const CommandIndex* index;
    ProtocolWriter writer;
    u32 optional;
};
Utf8View option_summary(const CommandIndex& index, const ApplicationCommand& command, Slice<char> output) noexcept {
    OptionSummary summary{&index, {{reinterpret_cast<u8*>(output.data), output.count}, 0}, 0};
    if (command.options.source.bytes)
        json_visit_indexed_array(
            index.document, command.options, 64,
            [](void* opaque, u32, JsonValue option) noexcept {
                auto& s = *static_cast<OptionSummary*>(opaque);
                bool required = false;
                json_value_to_bool(command_member(s.index->document, option, utf8_literal("required")), &required);
                if (!required) {
                    ++s.optional;
                    return true;
                }
                char storage[128];
                const Utf8View name = field_text(*s.index, option, "name", {storage, sizeof(storage)});
                if (name.bytes && s.writer.used + name.bytes + 2 <= s.writer.output.count) {
                    if (s.writer.used)
                        protocol_writer_append_bytes(&s.writer, {reinterpret_cast<const u8*>("  "), 2});
                    protocol_writer_append_bytes(&s.writer, {reinterpret_cast<const u8*>(name.data), name.bytes});
                }
                return true;
            },
            &summary);
    if (summary.optional && summary.writer.used + 24 <= output.count) {
        if (summary.writer.used)
            protocol_writer_append_bytes(&summary.writer, {reinterpret_cast<const u8*>("  "), 2});
        protocol_writer_append_byte(&summary.writer, '+');
        protocol_writer_append_u64(&summary.writer, summary.optional);
        protocol_writer_append_literal(&summary.writer, " optional");
    }
    return {output.data, summary.writer.used};
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
    // All, then Frequently Used (the client's clock section), then each app.
    const u32 extra = c.frequent_count || c.filter_app == frequent_filter ? 1u : 0u;
    for (u32 r = rail_first; !c.selected.command && r <= c.app_count + extra && count < 11; ++r) {
        if (extra && r == 1) {
            items[count] = {utf8_literal("Frequently Used"), rail_base + r,
                            popup_item_enabled | popup_item_command_category, 0};
            items[count].presentation = ui_menu_row_presentation(UiIcon::recent);
            if (c.filter_app == frequent_filter)
                items[count].flags |= popup_item_checked;
            ++count;
            continue;
        }
        auto* app = r ? c.apps[r - 1 - (r > 1 ? extra : 0)] : nullptr;
        items[count] = {app ? app->name : utf8_literal("All"), rail_base + r,
                        popup_item_enabled | popup_item_command_category, 0};
        if ((app ? app->id : 0) == c.filter_app)
            items[count].flags |= popup_item_checked;
        avatar(items[count], app);
        ++count;
    }
    if (!count && !c.selected.command)
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
    if (c.selected.command) {
        items[count] = {copy_label(c.labels[count], 512, utf8_literal("/"), c.selected.command->name),
                        choose_base - 2, popup_item_command_header, 0};
        items[count++].detail = c.error.bytes ? c.error : c.selected.command->description;
        if (c.focused_option.source.bytes) {
            items[0].label = field_text(*c.selected.index, c.focused_option, "name", {c.labels[0], 512});
            if (!c.error.bytes)
                items[0].detail = field_text(*c.selected.index, c.focused_option, "description", {c.details[0], 1024});
        }
        items[count++] = {utf8_literal("Clear command"), clear_selection,
                          popup_item_enabled | popup_item_command_close, 0};
        c.option_count = 0;
        c.choices_shown = render_choices(items, &count);
        if (c.choices_shown)
            c.option_count = c.choice_count;
        else if (c.selected.command->options.source.bytes) {
            OptionsUi options{&c, items, &count};
            json_visit_indexed_array(c.selected.index->document, c.selected.command->options, 64,
                                     option_row, &options);
        }
    } else if (c.error.bytes || (!c.count && !local_count)) {
        items[count++] = {c.error.bytes ? c.error
                          : c.pending   ? utf8_literal("Loading commands…")
                                        : utf8_literal("No matching commands"),
                          choose_base - 1, popup_item_command_row, 0};
    } else {
        const u32 first = static_cast<u32>(c.offset / row_height);
        for (u32 n = first; n < c.count && count < 23; ++n) {
            const auto& entry = c.entries[n];
            auto& item = items[count];
            item = {copy_label(c.labels[count], 512, utf8_literal("/"), entry.command->name),
                    choose_base + n, popup_item_enabled | popup_item_command_row, 0};
            // description \x1F app \x1F option summary (drawn as the client's row columns).
            char summary[512];
            const Utf8View options = option_summary(*entry.index, *entry.command, {summary, sizeof(summary)});
            const u32 used = copy_label(c.details[count], 1024, entry.command->description, utf8_literal("\x1F"),
                                        entry.app ? entry.app->name : Utf8View{})
                                 .bytes;
            item.detail = {c.details[count],
                           used + copy_label(c.details[count] + used, 1024 - used, utf8_literal("\x1F"), options).bytes};
            avatar(item, entry.app);
            ++count;
        }
    }
    const UiRect viewport = application_viewport_dips();
    const UiRect anchor = surface->rectangle;
    const auto result =
        popup_scene_open(&g_app.popup_scene, PopupKind::autocomplete, c.channel, {items, count}, anchor,
                         viewport, ui_rect_width(anchor), true,
                         c.selected.command ? c.offset :
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
// The deferred-response "thinking" flag the timeline draws as "<app> is thinking...".
constexpr u32 message_flag_loading = 1u << 7;
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
// fresh nonce, expects its modal and shows the official client's local
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
// The official client shows the API's reason for a quick refusal, and "did
// not respond" once the response window has passed.
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
    return false;
}
// Option type of one entry of a command's options array (0 when unreadable).
u64 option_type(const CommandIndex& index, JsonValue option) noexcept {
    u64 type = 0;
    json_value_to_u64(command_member(index.document, option, utf8_literal("type")), &type);
    return type;
}
struct AttachmentProbe {
    const CommandIndex* index;
    bool found;
};
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
} // namespace
void application_commands_composer(MessageComposerProjectionContext* projection) noexcept {
    reset_value(&projection->command);
    projection->option_key_count = 0;
    if (!commands || !commands->selected.command || g_app.message_edit.editor_active)
        return;
    const auto& c = *commands;
    const auto input = message_composer_text(g_app.message_composer);
    const auto name = c.selected.command->name;
    if (name.bytes >= 0xffffu || c.channel != g_app.message_composer.channel_id || input.bytes < name.bytes + 1 ||
        !input.bytes || input.data[0] != '/' || !utf8_equal({input.data + 1, name.bytes}, name) ||
        (input.bytes > name.bytes + 1 && !command_space(input.data[name.bytes + 1])))
        return;
    projection->command.name_bytes = static_cast<u16>(name.bytes + 1);
    // The typed option keys draw as the client's option pills.
    const u32 start = name.bytes + 1;
    if (input.bytes > start && input.bytes < 0xffffu) {
        CommandArgumentKey keys[25];
        const u32 count = command_argument_keys(*c.selected.index, *c.selected.command,
                                                {input.data + start, input.bytes - start}, {keys, 25});
        for (u32 n = 0; n < count; ++n) {
            projection->option_key_first[n] = static_cast<u16>(start + keys[n].first);
            projection->option_key_last[n] = static_cast<u16>(start + keys[n].last);
        }
        projection->option_key_count = count;
    }
    if (c.selected.app && c.selected.app->bot_id && g_app.account_ui) {
        const auto* user = user_store_find(g_app.account_ui->user_store, c.selected.app->bot_id);
        if (user) {
            const auto hash = user_store_avatar_hash(*user);
            projection->command.bot_id = user->id;
            projection->command.avatar_variant = user_avatar_variant(hash);
            projection->command.animated = hash.bytes > 2 && hash.data[0] == 'a' && hash.data[1] == '_';
        }
    }
}
bool application_commands_sync(bool* handled) noexcept {
    *handled = false;
    const auto input = message_composer_text(g_app.message_composer);
    if (!g_app.account_ui || !input.bytes || input.data[0] != '/' || g_app.message_edit.editor_active) {
        close_popup();
        if (commands) {
            reset_value(&commands->selected);
            commands->revision = 0;
        }
        return true;
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
        return true;
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
            (input.bytes > name.bytes + 1 && !command_space(input.data[name.bytes + 1])))
            reset_value(&c.selected);
    }
    resolve_wanted(input);
    const bool changed = c.revision != g_app.message_composer.revision;
    if (!c.selected.command)
        reset_value(&c.focused_option);
    if (changed) {
        reset_value(&c.focused_option);
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
        reset_value(&c.focused_option);
        c.choice_count = c.option_count = 0;
        command_autocomplete_cancel(&c.autocomplete);
        c.offset = c.rail_offset = 0;
        c.revision = 0;
        edit_result = message_composer_clear(&g_app.message_composer);
    } else if (action >= plugin_base) {
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
        // Names with separators are quoted so the argument scanner keeps them whole.
        bool quote = type == 3;
        for (u32 at = 0; at < chosen.bytes && !quote; ++at)
            quote = command_space(chosen.data[at]) || chosen.data[at] == ':' || chosen.data[at] == '"';
        bool ok = quote ? protocol_writer_append_json_string(&writer, chosen)
                        : protocol_writer_append_bytes(
                              &writer, {reinterpret_cast<const u8*>(chosen.data), chosen.bytes});
        const auto input = message_composer_text(g_app.message_composer);
        if (c.value_end == input.bytes)
            ok = ok && protocol_writer_append_byte(&writer, ' ');
        if (ok && c.value_begin <= c.value_end && c.value_end <= input.bytes)
            edit_result = message_composer_replace_range(&g_app.message_composer, c.value_begin,
                                                         c.value_end, {insertion, writer.used});
        c.accepted_revision = g_app.message_composer.revision;
        reset_value(&c.focused_option);
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
        // An ATTACHMENT option asks for its file, like the client's upload pill.
        if (option_type(*c.selected.index, pick.value) == 11 && !application_pick_attachments())
            return false;
    } else if (action >= rail_base) {
        const u32 n = action - rail_base;
        const u32 extra = c.frequent_count || c.filter_app == frequent_filter ? 1u : 0u;
        if (n > c.app_count + extra)
            return true;
        c.filter_app = !n ? 0 : extra && n == 1 ? frequent_filter : c.apps[n - 1 - extra]->id;
        reset_value(&c.selected);
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
    // ATTACHMENT options take the channel's staged uploads (the client stages
    // them the same way, through the channel attachments endpoint).
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
                           {files, upload_count}, &plan))
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
        // The uploads now belong to this command; like the client, a command with
        // files offers no resend.
        application_attachment_did_queue({staged_ids, plan.count}, c.nonce);
        if (Echo* echo = find_echo(c.nonce, 0))
            echo->data_bytes = 0;
    }
    command_autocomplete_cancel(&c.autocomplete);
    reset_value(&c.error);
    reset_value(&c.selected);
    close_popup();
    // Like the official client, the draft clears as the command is sent; its
    // outcome is the local command message.
    return composer_apply_result(message_composer_clear(&g_app.message_composer), true);
}
bool application_commands_pending(u64 now) noexcept {
    if (!commands || !g_app.account_ui || !g_app.account.api.application_command) return false;
    const auto& c = *commands;
    const bool suggest = c.autocomplete.phase == CommandAutocompletePhase::debounce && now >= c.autocomplete.due_at;
    const bool lane = c.queued || ((c.pending || suggest) &&
                                   (c.context_wanted || g_app.popup_owner_kind == PopupOwnerKind::commands));
    return !c.sending && c.retry_at <= now && (lane || frecency_fetch_due(now) || frecency_save_due(now));
}
bool application_commands_submit(u64 now) noexcept {
    if (!application_commands_pending(now)) return false;
    auto& c = *commands;
    const u8 slot = (c.pending & 1) ? 0 : 1;
    const bool popup = g_app.popup_owner_kind == PopupOwnerKind::commands;
    const bool indexes = c.pending && (c.context_wanted || popup);
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
    const bool same = c.requested_channel == c.channel && c.requested_guild == c.guild;
    if (c.operation == 0 && c.requested_index_epoch != c.channel_index_epoch)
        return refresh_message_composer_ui();
    if (c.operation == 3) {
        c.suggestions_dirty =
            command_autocomplete_transport(&c.autocomplete, c.issued_nonce,
                                           response.transport_ok() && response.status_code >= 200 &&
                                               response.status_code < 300) ||
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
        const bool ok = response.transport_ok() && response.status_code >= 200 && response.status_code < 300 &&
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
        if (!response.transport_ok() || response.status_code < 200 || response.status_code >= 300) {
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
    const auto* styles = g_app.popup_scene.theme_styles;
    const float padding = c.selected.command && styles && styles->ready ?
        styles->metrics.menu_padding : kPopupPanelPadding;
    auto rect = g_app.popup_scene.rectangle;
    rect.left += padding;
    rect.right -= padding;
    rect.top += padding;
    rect.bottom -= padding;
    if (c.selected.command) {
        if (rail || !c.option_count)
            return false;
        // The compact summary and its close control stay above the options.
        rect.top += styles && styles->ready ? styles->metrics.menu_row_height : kUiMenuRowHeight;
        const double extent = option_row_height();
        const double height = c.option_count * extent;
        *range = {rect, c.offset, maximum(0.0, height - ui_rect_height(rect)), extent,
                  1, 0, c.generation};
        return true;
    }
    if (rail)
        rect.right = minimum(rect.right, rect.left + 48);
    else
        rect.left += 48;
    const double extent = rail ? rail_height : row_height;
    const double height =
        rail ? (c.app_count + 1 + (c.frequent_count || c.filter_app == frequent_filter ? 1 : 0)) * extent
             : (c.selected.command ? c.option_count + 1 : c.count) * extent;
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
        const float extent = c.selected.command ? option_row_height() : row_height;
        const float top = (c.selected.command ? c.keyboard_row - 1 : c.keyboard_row) * extent;
        return reveal_command_row(top, top + extent) && render();
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
    if (c.selected.index == &c.index[0]) {
        reset_value(&c.selected);
        reset_value(&c.focused_option);
        c.choice_count = c.option_count = 0;
        command_autocomplete_cancel(&c.autocomplete);
    }
    // Visible context-menu labels can still borrow this arena. Retire it until
    // post-drain UI refresh, while making the stale commands unselectable now.
    if (c.index[0].storage.base)
        c.retired_index_storage = c.index[0].storage;
    reset_value(&c.index[0]);
    arena_release(&c.projection);
    c.entries = nullptr;
    c.apps = nullptr;
    c.count = c.app_count = c.frequent_count = 0;
    c.filter_app = 0;
    c.revision = 0;
    c.catalog_dirty = true;
    ++c.catalog_revision;
    ++c.generation;
    // This close is a cache refresh, not a dismissal of the unchanged draft.
    const u64 dismissed_document = c.dismissed_document;
    const u32 dismissed_revision = c.dismissed_revision;
    close_popup();
    c.dismissed_document = dismissed_document;
    c.dismissed_revision = dismissed_revision;
}
bool application_commands_tick(u64 now) noexcept {
    if (!commands)
        return true;
    auto& c = *commands;
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
    if (!idle)
        return wake;
    if (c.suggestions_dirty)
        return 1;
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
        (!c.queued && (!c.pending || (g_app.popup_owner_kind != PopupOwnerKind::commands && !c.context_wanted))))
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
        PostMessageW(g_app.window, WM_NULL, 0, 0);
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
    // Like the client: the failed local message goes and the same command data
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
        PostMessageW(g_app.window, WM_NULL, 0, 0);
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
    // The official client asks before replacing a written draft.
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
    if (!commands)
        return;
    command_autocomplete_cancel(&commands->autocomplete);
    command_index_release(&commands->index[0]);
    command_index_release(&commands->index[1]);
    arena_release(&commands->projection);
    arena_release(&commands->retired_index_storage);
    VirtualFree(commands, 0, MEM_RELEASE);
    commands = nullptr;
    if (frecency)
        VirtualFree(frecency, 0, MEM_RELEASE);
    frecency = nullptr;
}
} // namespace maicord::app_detail
