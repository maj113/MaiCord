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
#include "application_builtin_commands.hpp"
#include "maicord/protocol/message_interaction.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/model/user_activity.hpp"
#include "maicord/model/user_profile.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/ui/popup_presentation.hpp"
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
// The picker's sections: MaiCord's local plugin commands, Frequently Used, each
// app (by name) and Built-in last.
enum class SectionKind : u8 { plugins, frequent, app, builtin };
struct Section {
    const CommandApplication* app; // null for plugins and Frequently Used
    u32 first, count;              // rows in entries (plugins: the local list)
    u32 placeholders;              // loading rows after them
    SectionKind kind;
};
constexpr u32 plugin_rows = 8;
// The lane's built-in request (the API operation is the action's own).
constexpr u8 builtin_operation = 9;
constexpr u32 no_row = ~0u;
// The query results' size cap and the scroll-spy's lead.
constexpr u32 query_limit = 20;
constexpr float spy_lead = 20;
// While an index loads, discovery shows seven placeholder rows and query results three.
constexpr u32 discovery_placeholder_rows = 7, query_placeholder_rows = 3;
// The local command message ("<user> used <command>") while
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
    // Picker rows in body order (discovery: section by section; a query:
    // ranked), the rail's sections, the body's sections (the filtered one
    // alone) and the local plugin rows leading the body.
    Entry* entries;
    u32 count;
    Section *rail, *body;
    u32 rail_count, body_count, plugin_count, query_placeholders;
    // The query text the results answer ("/" prefixed in their title).
    char query[128];
    u32 query_bytes;
    Entry selected;
    CommandAutocomplete autocomplete;
    u64 issued_nonce, nonce_serial;
    bool suggestions_dirty, echo_dirty;
    // The option chip the caret is in (the active option): its
    // definition, slot and raw value range in the draft; no_option outside.
    JsonValue focused_option;
    u32 focused_slot, value_begin, value_end, choice_count;
    bool choices_shown;
    // The rows of the focused option's choice list: the names put in the draft,
    // and for autocomplete rows the suggestion slot that holds the value.
    char choice_names[25][512];
    u32 choice_bytes[25];
    bool choice_suggested;
    // Option states: each option's last check (empty error when
    // it passed or never ran) and its value when last seen; the active option
    // at the previous sync is checked once the caret leaves it.
    struct OptionState {
        Utf8View error;
        u64 value_hash;
        char message[160];
    } option_states[25];
    u32 previous_slot;
    // Autocomplete choices taken, one per option.
    CommandAcceptedChoice accepted[25];
    char accepted_text[25][3][512];
    u32 accepted_count;
    // The command whose required options were set up as chips; the rows of
    // the selected command's list under its option bar.
    const ApplicationCommand* prepared;
    u32 focused_revision;
    // The selected command's list content (title gaps included, without its bottom inset).
    float list_height;
    // The composer's option bar text (scenes copy it).
    char bar_name[128], bar_detail[1024];
    // The app shake of a refused send: its start, next frame, frame count,
    // horizontal direction and random state.
    u64 shake_started, shake_next;
    u32 shake_frame, shake_seed;
    float shake_direction;
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
    // The keyboard selection (no_row: none, as after the pointer hovers a row).
    // An empty query is the discovery list (no initial selection,
    // arrows stop at the ends); a typed query and the option lists wrap.
    // rail_focus is the rail section the keyboard moved into (no_row: none).
    u32 keyboard_row, option_count, rail_focus;
    bool discovery;
    u8 attempts[2];
    Utf8View error;
    char labels[32][512], details[32][1024], payload[65536];
    u32 payload_bytes;
    u64 nonce, send_application, send_channel, send_guild;
    // A context menu wants the index loaded.
    bool context_wanted;
    // A built-in text command rewrote the draft at this revision; it sends as
    // an ordinary message.
    bool builtin_text;
    u32 builtin_text_revision;
    Echo echoes[echo_slots];
    u32 echo_next;
    // Pictures fetched by URL (app icons, bots' default avatars) that picker
    // items name by the URL's hash; a ring past the visible rail and rows.
    struct IconUrl {
        u64 hash;
        u16 bytes;
        char url[160];
    } icon_urls[64];
    u32 icon_url_next;
};
Commands* commands;
Commands* ensure_commands() noexcept {
    if (!commands)
        commands = static_cast<Commands*>(
            VirtualAlloc(nullptr, sizeof(Commands), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return commands;
}
// Command frecency (settings-proto/2): uses
// are tracked locally, saved a few seconds later and replayed over a fresh
// copy until a save confirms them. Allocated with the first picker or use.
constexpr u32 frecency_pending_uses = kCommandFrecencyPendingUses, frequently_shown = 5, frequently_ranked = 100;
constexpr u64 frecency_save_delay_ms = 5000, frecency_retry_ms = 30000;
struct Frecency {
    CommandFrecencyEntry entries[kCommandFrecencyMaximumEntries];
    CommandFrecencyUse pending[frecency_pending_uses];
    u32 count, data_version, pending_count, sent_pending;
    u64 revision, save_due, retry_after;
    // The profile whose unsaved uses were read from disk.
    u64 store_profile;
    bool loaded, fetching, saving, store_read;
    u8 scratch[64 * 1024];
    u8 json[96 * 1024];
};
Frecency* frecency;
// Unsaved uses outlive a restart: one small file per account profile.
bool frecency_store_path(u64 profile, wchar_t* path, u32 capacity) noexcept {
    if (!profile)
        return false;
    const DataPathResult built = data_path(path, capacity, L"accounts\\command-uses-");
    u32 length = built.length;
    return built.ok() && wide_append_hex(path, capacity, &length, profile) &&
           wide_append(path, capacity, &length, L".pending");
}
void frecency_store_write() noexcept {
    const auto& f = *frecency;
    wchar_t path[512];
    u8 record[kCommandFrecencyPendingFileBytes];
    const u32 bytes = command_frecency_pending_encode({f.pending, f.pending_count}, {record, sizeof(record)});
    const u32 length = frecency_store_path(f.store_profile, path, 512) ? wide_length(path, 512) : 0;
    if (bytes && length && !file_persistence_write_atomic(path, length, {record, bytes}).ok())
        log_write(LogLevel::warning, "commands", "frecency_pending_write_failed");
}
// Reads the profile's unsaved uses once (again after a profile change).
void frecency_store_read(Frecency& f) noexcept {
    const u64 profile = g_app.account.status.profile_id;
    if (f.store_read && f.store_profile == profile)
        return;
    f.store_read = true;
    f.store_profile = profile;
    f.pending_count = f.sent_pending = 0;
    wchar_t path[512];
    u8 record[kCommandFrecencyPendingFileBytes];
    if (!frecency_store_path(profile, path, 512))
        return;
    const FileReadResult read = file_persistence_read_file(path, {record, sizeof(record)});
    u32 count = 0;
    if (read.ok() && command_frecency_pending_decode({record, read.bytes}, {f.pending, frecency_pending_uses}, &count))
        f.pending_count = count;
    if (f.pending_count && !f.save_due)
        f.save_due = clock_monotonic_milliseconds() + frecency_save_delay_ms;
}
Frecency* ensure_frecency() noexcept {
    if (!frecency)
        frecency = static_cast<Frecency*>(
            VirtualAlloc(nullptr, sizeof(Frecency), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (frecency)
        frecency_store_read(*frecency);
    return frecency;
}
bool frecency_current() noexcept {
    return frecency && frecency->loaded && frecency->revision == g_app.account.status.frecency_settings_revision;
}
// The frecency key: a built-in's negative id; otherwise the id, "\0group",
// "\0sub", and ":<guild>" for a server's own command used in that server.
Utf8View frecency_key(const ApplicationCommand& command, u64 guild, Slice<char> output) noexcept {
    ProtocolWriter w{{reinterpret_cast<u8*>(output.data), output.count}, 0};
    if (command.application_id == kBuiltinCommandApplication) {
        const bool ok = protocol_writer_append_byte(&w, '-') && protocol_writer_append_u64(&w, command.id);
        return ok ? Utf8View{output.data, w.used} : Utf8View{};
    }
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
        copy_bytes(f->pending, f->pending + 1, (frecency_pending_uses - 1) * sizeof(CommandFrecencyUse));
        --f->pending_count;
        f->sent_pending = f->sent_pending ? f->sent_pending - 1 : 0;
    }
    auto& use = f->pending[f->pending_count++];
    use.at = now;
    use.key_bytes = static_cast<u8>(key.bytes);
    copy_bytes(use.key, key.data, key.bytes);
    frecency_store_write();
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
    copy_bytes(f.pending, f.pending + confirmed, (f.pending_count - confirmed) * sizeof(CommandFrecencyUse));
    f.pending_count -= confirmed;
    if (confirmed)
        frecency_store_write();
    for (u32 n = 0; n < f.pending_count; ++n)
        command_frecency_track({f.entries, kCommandFrecencyMaximumEntries}, &f.count,
                               {f.pending[n].key, f.pending[n].key_bytes}, f.pending[n].at);
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
// The command's entry in the account's command frecency. The picker reads the
// copy it has while a newer one is fetched.
const CommandFrecencyEntry* frecency_entry(const ApplicationCommand& command, u64 guild) noexcept {
    if (!frecency || !frecency->loaded)
        return nullptr;
    char storage[kCommandFrecencyKeyBytes + 32];
    const Utf8View key = frecency_key(command, guild, {storage, sizeof(storage)});
    const auto& f = *frecency;
    for (u32 n = 0; n < f.count; ++n)
        if (f.entries[n].key_bytes == key.bytes && bytes_equal(f.entries[n].key, key.data, key.bytes))
            return f.entries + n;
    return nullptr;
}
// Score (hundredths) of a command among the 100 most frecent entries, or 0.
i32 frequently_score(const ApplicationCommand& command, u64 guild) noexcept {
    const auto* entry = frecency_entry(command, guild);
    if (!entry)
        return 0;
    u32 ahead = 0;
    for (u32 other = 0; other < frecency->count && ahead < frequently_ranked; ++other)
        ahead += frecency->entries[other].frecency > entry->frecency ? 1u : 0u;
    return ahead < frequently_ranked ? maximum(1, entry->score) : 0;
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
Utf8View copy_label(char* out, u32 capacity, Utf8View a, Utf8View b = {}, Utf8View c = {},
                    Utf8View d = {}) noexcept {
    u32 n = 0;
    const Utf8View parts[] = {a, b, c, d};
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
// The section name: the bot's username, else the app's name.
Utf8View section_name(const CommandApplication& app) noexcept {
    return app.bot_name.bytes ? app.bot_name : app.name;
}
bool starts_with_ignore_case(Utf8View text, Utf8View prefix) noexcept {
    return text.bytes >= prefix.bytes && ascii_equal_ignore_case({text.data, prefix.bytes}, prefix);
}
// Fuzzy match: needle's characters appear in order in text.
bool fuzzy_ignore_case(Utf8View needle, Utf8View text) noexcept {
    if (needle.bytes > text.bytes)
        return false;
    if (needle.bytes == text.bytes)
        return ascii_equal_ignore_case(needle, text);
    u32 at = 0;
    for (u32 n = 0; n < needle.bytes; ++n) {
        while (at < text.bytes && ascii_lower(text.data[at]) != ascii_lower(needle.data[n]))
            ++at;
        if (at++ == text.bytes)
            return false;
    }
    return true;
}
// Collator order, approximated case-insensitively.
i32 compare_names(Utf8View a, Utf8View b) noexcept {
    const u32 shared = minimum(a.bytes, b.bytes);
    for (u32 n = 0; n < shared; ++n) {
        const u8 x = ascii_lower(static_cast<u8>(a.data[n])), y = ascii_lower(static_cast<u8>(b.data[n]));
        if (x != y)
            return x < y ? -1 : 1;
    }
    return a.bytes == b.bytes ? 0 : a.bytes < b.bytes ? -1 : 1;
}
// The query text: before the word ahead of a
// "name:" argument, at most three words; a cut or a trailing space makes it
// "terminated" (only that command and its subcommands match).
struct PickerQuery {
    Utf8View text;
    bool terminated;
};
PickerQuery picker_query(Utf8View input) noexcept {
    u32 end = input.bytes;
    for (u32 at = 0; at < input.bytes; ++at)
        if (input.data[at] == ':') {
            end = at;
            while (end && input.data[end - 1] != ' ')
                --end;
            end = end ? end - 1 : at;
            break;
        }
    for (u32 at = 0, spaces = 0; at < end; ++at)
        if (input.data[at] == ' ' && ++spaces == 3) {
            end = at;
            break;
        }
    PickerQuery query{{input.data, end}, end < input.bytes};
    while (query.text.bytes && command_space(query.text.data[query.text.bytes - 1])) {
        --query.text.bytes;
        query.terminated = true;
    }
    return query;
}
// The query score (lower ranks first): name prefix 0,
// first-word and rest prefixes 1, name substring 2, option prefix 3, option
// substring 4, section prefix 5 or substring 6, description substring 7,
// fuzzy name 9, option 10, section 11, description 12; ~0u: no match.
struct OptionMatch {
    const CommandIndex* index;
    Utf8View name, query;
    u32 score;
};
u32 query_score(const Entry& entry, Utf8View query) noexcept {
    const auto& command = *entry.command;
    const Utf8View name = command.name;
    u32 score = ~0u;
    if (entry.app) {
        const Utf8View section = section_name(*entry.app);
        score = starts_with_ignore_case(section, query)  ? 5
                : ascii_contains_ignore_case(section, query) ? 6
                : fuzzy_ignore_case(query, section)          ? 11
                                                             : ~0u;
    }
    u32 first_word = query.bytes;
    for (u32 at = 0; at < query.bytes; ++at)
        if (query.data[at] == ' ') {
            first_word = at;
            break;
        }
    u32 name_word = name.bytes;
    for (u32 at = 0; at < name.bytes; ++at)
        if (name.data[at] == ' ') {
            name_word = at;
            break;
        }
    const Utf8View head{query.data, first_word};
    const Utf8View rest = first_word < query.bytes ? Utf8View{query.data + first_word + 1, query.bytes - first_word - 1}
                                                   : Utf8View{};
    const Utf8View name_rest = name_word < name.bytes ? Utf8View{name.data + name_word + 1, name.bytes - name_word - 1}
                                                      : Utf8View{};
    // Option names: prefix 3 (alone or after the command name), substring 4,
    // fuzzy 10.
    OptionMatch options{entry.index, name, query, ~0u};
    if (command.options.source.bytes)
        json_visit_indexed_array(
            entry.index->document, command.options, 64,
            [](void* opaque, u32, JsonValue option) noexcept {
                auto& m = *static_cast<OptionMatch*>(opaque);
                char storage[128];
                const Utf8View option_name = field_text(*m.index, option, "name", {storage, sizeof(storage)});
                if (!option_name.bytes)
                    return true;
                const bool after_name = m.query.bytes > m.name.bytes &&
                                        starts_with_ignore_case(m.query, m.name) && m.query.data[m.name.bytes] == ' ' &&
                                        starts_with_ignore_case(option_name, {m.query.data + m.name.bytes + 1,
                                                                              m.query.bytes - m.name.bytes - 1});
                const u32 score = starts_with_ignore_case(option_name, m.query) || after_name ? 3
                                  : ascii_contains_ignore_case(option_name, m.query)            ? 4
                                  : fuzzy_ignore_case(m.query, option_name)                     ? 10
                                                                                                : ~0u;
                m.score = minimum(m.score, score);
                return true;
            },
            &options);
    const u32 command_score =
        starts_with_ignore_case(name, query)                                                        ? 0
        : starts_with_ignore_case(name, head) && starts_with_ignore_case(name_rest, rest)           ? 1
        : ascii_contains_ignore_case(name, query)                                                  ? 2
        : options.score <= 4                                                                       ? options.score
        : ascii_contains_ignore_case(command.description, query)                                   ? 7
        : fuzzy_ignore_case(query, name)                                                           ? 9
        : options.score == 10                                                                      ? 10
        : fuzzy_ignore_case(query, command.description)                                            ? 12
                                                                                                   : ~0u;
    return minimum(score, command_score);
}
// Account frecency score (hundredths) of a command, 0 when never used.
i32 frecency_score(const ApplicationCommand& command, u64 guild) noexcept {
    const auto* entry = frecency_entry(command, guild);
    return entry ? entry->score : 0;
}
bool project(Utf8View input) noexcept {
    auto& c = *commands;
    const PickerQuery query = picker_query(input);
    arena_release(&c.projection);
    c.entries = nullptr;
    c.rail = c.body = nullptr;
    c.count = c.rail_count = c.body_count = 0;
    c.query_bytes = utf8_copy_truncated(c.query, sizeof(c.query), query.text);
    NativePluginSdkCommandView local[plugin_rows];
    c.plugin_count = application_plugins_commands(command_query(input), {local, plugin_rows});
    const CommandIndex* builtins = builtin_commands_index();
    const u32 capacity =
        c.index[0].command_count + c.index[1].command_count + (builtins ? builtins->command_count : 0);
    // Candidates, their ranks and the apps, then the rows and both section lists.
    const usize bytes = static_cast<usize>(capacity) * (2 * sizeof(Entry) + 2 * sizeof(u32) + sizeof(void*)) +
                        static_cast<usize>(capacity + 3) * 2 * sizeof(Section) + frequently_shown * sizeof(Entry) +
                        4096;
    if (!arena_initialize(&c.projection, bytes, 0, 4096))
        return false;
    Entry* candidates = arena_allocate<Entry>(&c.projection, maximum(1u, capacity));
    u32* ranks = arena_allocate<u32>(&c.projection, maximum(1u, capacity));
    u32* order = arena_allocate<u32>(&c.projection, maximum(1u, capacity));
    const CommandApplication** apps = arena_allocate<const CommandApplication*>(&c.projection, maximum(1u, capacity));
    c.entries = arena_allocate<Entry>(&c.projection, capacity + frequently_shown);
    c.rail = arena_allocate<Section>(&c.projection, capacity + 3);
    c.body = arena_allocate<Section>(&c.projection, capacity + 3);
    if (!candidates || !ranks || !order || !apps || !c.entries || !c.rail || !c.body)
        return false;
    u32 candidate_count = 0, app_count = 0;
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
            for (u32 j = 0; j < candidate_count && !duplicate; ++j)
                duplicate = candidates[j].command->id == command.id &&
                            utf8_equal(candidates[j].command->name, command.name);
            if (duplicate)
                continue;
            const auto* app = find_app(source, command.application_id);
            if (!app)
                continue;
            bool seen = false;
            for (u32 j = 0; j < app_count && !seen; ++j)
                seen = apps[j]->id == app->id;
            if (!seen)
                apps[app_count++] = app;
            candidates[candidate_count++] = {&command, &source, app};
        }
    // Apps by section name; Built-in comes last.
    heap_sort(apps, app_count, [](const CommandApplication* a, const CommandApplication* b) noexcept {
        const i32 order = compare_names(section_name(*a), section_name(*b));
        return order ? order < 0 : a->id < b->id;
    });
    const u32 first_builtin = candidate_count;
    for (u32 n = 0; builtins && n < builtins->command_count; ++n)
        if (builtin_command_available(builtins->commands[n], c.channel))
            candidates[candidate_count++] = {builtins->commands + n, builtins, builtins->applications};
    for (u32 n = 0; n < candidate_count; ++n) {
        order[n] = n;
        u32 rank = app_count;
        for (u32 j = 0; n < first_builtin && j < app_count; ++j)
            if (apps[j]->id == candidates[n].app->id) {
                rank = j;
                break;
            }
        ranks[n] = rank;
    }
    c.discovery = !query.text.bytes;
    if (!c.discovery) {
        // Query results: ranked by score, then frecency, then name, at most twenty.
        u32 kept = 0;
        for (u32 n = 0; n < candidate_count; ++n) {
            const auto& name = candidates[n].command->name;
            if (query.terminated &&
                !(ascii_equal_ignore_case(name, query.text) ||
                  (name.bytes > query.text.bytes && starts_with_ignore_case(name, query.text) &&
                   name.data[query.text.bytes] == ' ')))
                continue;
            const u32 score = query_score(candidates[n], query.text);
            if (score == ~0u)
                continue;
            ranks[kept] = score;
            order[kept++] = n;
        }
        struct Rank {
            const Entry* candidates;
            const u32* scores;
            const u32* order;
            u64 guild;
        } rank{candidates, ranks, order, c.guild};
        u32 slots[query_limit + 1];
        u32 slot_count = 0;
        // Keep the best twenty with an insertion pass (positions into order/ranks).
        const auto better = [&rank](u32 a, u32 b) noexcept {
            if (rank.scores[a] != rank.scores[b])
                return rank.scores[a] < rank.scores[b];
            const auto& x = rank.candidates[rank.order[a]];
            const auto& y = rank.candidates[rank.order[b]];
            const i32 fx = frecency_score(*x.command, rank.guild), fy = frecency_score(*y.command, rank.guild);
            if (fx != fy)
                return fx > fy;
            const i32 names = compare_names(x.command->name, y.command->name);
            return names ? names < 0 : rank.order[a] < rank.order[b];
        };
        for (u32 n = 0; n < kept; ++n) {
            u32 at = slot_count < query_limit ? slot_count++ : query_limit;
            while (at && better(n, slots[at - 1])) {
                if (at < query_limit)
                    slots[at] = slots[at - 1];
                --at;
            }
            if (at < query_limit)
                slots[at] = n;
        }
        for (u32 n = 0; n < slot_count; ++n)
            c.entries[c.count++] = candidates[order[slots[n]]];
    } else {
        // Discovery: each app's commands by name, then Frequently
        // Used (up to five by score among its hundred most frecent) ahead.
        heap_sort(order, candidate_count, [&](u32 a, u32 b) noexcept {
            if (ranks[a] != ranks[b])
                return ranks[a] < ranks[b];
            const i32 names = compare_names(candidates[a].command->name, candidates[b].command->name);
            return names ? names < 0 : a < b;
        });
        u32 picked[frequently_shown]{};
        i32 scores[frequently_shown]{};
        u32 frequent = 0;
        for (u32 n = 0; n < candidate_count; ++n) {
            const i32 score = frequently_score(*candidates[order[n]].command, c.guild);
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
                picked[at] = order[n];
                scores[at] = score;
            }
        }
        if (frequent)
            c.rail[c.rail_count++] = {nullptr, 0, frequent, 0, SectionKind::frequent};
        for (u32 n = 0; n < candidate_count;) {
            const u32 rank = ranks[order[n]];
            u32 end = n;
            while (end < candidate_count && ranks[order[end]] == rank)
                ++end;
            c.rail[c.rail_count++] = {candidates[order[n]].app, frequent + n, end - n, 0,
                                      rank == app_count ? SectionKind::builtin : SectionKind::app};
            n = end;
        }
        // A rail click filters the body to that section alone.
        const Section* filtered = nullptr;
        for (u32 s = 0; s < c.rail_count && c.filter_app && !filtered; ++s)
            if (c.rail[s].app && c.rail[s].app->id == c.filter_app)
                filtered = c.rail + s;
        if (!filtered)
            c.filter_app = 0;
        if (filtered) {
            for (u32 n = 0; n < filtered->count; ++n)
                c.entries[n] = candidates[order[filtered->first - frequent + n]];
            c.count = filtered->count;
            c.body[c.body_count++] = {filtered->app, 0, filtered->count, 0, filtered->kind};
        } else {
            if (c.plugin_count)
                c.body[c.body_count++] = {nullptr, 0, c.plugin_count, 0, SectionKind::plugins};
            for (u32 n = 0; n < frequent; ++n)
                c.entries[c.count++] = candidates[picked[n]];
            for (u32 n = 0; n < candidate_count; ++n)
                c.entries[c.count++] = candidates[order[n]];
            for (u32 s = 0; s < c.rail_count; ++s)
                c.body[c.body_count++] = c.rail[s];
        }
        // While an index loads, the first section ends in placeholder rows.
        for (u32 s = 0; c.pending && s < c.body_count; ++s)
            if (c.body[s].kind != SectionKind::plugins) {
                c.body[s].placeholders = discovery_placeholder_rows;
                break;
            }
    }
    c.query_placeholders = !c.discovery && c.pending ? query_placeholder_rows : 0;
    c.offset = c.rail_offset = 0;
    c.rail_focus = no_row;
    // The discovery list starts without a selection; results of a
    // typed query focus their first row.
    c.keyboard_row = c.discovery ? no_row : 0;
    ++c.generation;
    return true;
}
// Keeps a picture's URL for the media pump under its key (the URL's hash),
// fetched as a still image of format. Kind none when the URL does not fit.
MediaAssetKey url_picture(Utf8View url, MediaAttachmentFormat format) noexcept {
    auto& c = *commands;
    if (!url.bytes || url.bytes > sizeof(c.icon_urls[0].url))
        return {};
    const u64 hash = hash64_bytes(kHash64Seed, url.data, url.bytes);
    bool known = false;
    for (const auto& entry : c.icon_urls)
        known = known || (entry.hash == hash && entry.bytes);
    if (!known) {
        auto& entry = c.icon_urls[c.icon_url_next++ % (sizeof(c.icon_urls) / sizeof(c.icon_urls[0]))];
        entry.hash = hash;
        entry.bytes = static_cast<u16>(url.bytes);
        copy_bytes(entry.url, url.data, url.bytes);
    }
    return popup_media_url_key(hash, format);
}
// The app's picture for the picker and the composer's command bar alike: the
// bot's avatar in this server (its member record), its avatar, the
// application's icon (app-icons), then the bot's default avatar; all still.
// An index bot not yet known is made resident from the index, as the local
// command message does; one known without an avatar takes the index's.
MediaAssetKey app_picture(const CommandApplication* app) noexcept {
    if (!app || !g_app.account_ui)
        return {};
    const auto& c = *commands;
    auto& users = g_app.account_ui->user_store;
    char url[192];
    ProtocolWriter w{{reinterpret_cast<u8*>(url), sizeof(url)}, 0};
    if (c.guild && app->bot_id)
        if (const auto* member = user_appearance_find(users.appearance, app->bot_id, c.guild)) {
            const Utf8View hash = member->avatar.view();
            if (user_avatar_variant(hash) &&
                protocol_writer_append_c_string(&w, "https://cdn.discordapp.com/guilds/") &&
                protocol_writer_append_u64(&w, c.guild) && protocol_writer_append_c_string(&w, "/users/") &&
                protocol_writer_append_u64(&w, app->bot_id) && protocol_writer_append_c_string(&w, "/avatars/") &&
                protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(hash.data), hash.bytes) &&
                protocol_writer_append_c_string(&w, ".webp?size=64"))
                return url_picture({url, w.used}, MediaAttachmentFormat::webp);
        }
    const UserRecord* user = nullptr;
    if (app->bot_id) {
        if (!user_store_find(users, app->bot_id))
            user_store_upsert(&users, app->bot_id, section_name(*app));
        user = user_store_find(users, app->bot_id);
        if (user && !user_store_avatar_hash(*user).bytes && app->avatar.bytes &&
            user_store_set_avatar(&users, app->bot_id, app->avatar))
            user = user_store_find(users, app->bot_id);
    }
    const Utf8View hash = user ? user_store_avatar_hash(*user) : Utf8View{};
    if (const u32 variant = user ? user_avatar_variant(hash) : 0)
        return media_asset_user_avatar(user->id, variant, asset_hash_animated(hash));
    if (const u32 bytes = user_activity_application_icon_url(app->id, app->icon, {url, sizeof(url)}))
        return url_picture({url, bytes}, MediaAttachmentFormat::png);
    if (!app->bot_id)
        return {};
    // The CDN's default avatar for the bot ((id >> 22) % 6, as profiles use).
    static constexpr char prefix[] = "https://cdn.discordapp.com/embed/avatars/";
    constexpr u32 prefix_bytes = sizeof(prefix) - 1;
    copy_bytes(url, prefix, prefix_bytes);
    url[prefix_bytes] = static_cast<char>('0' + (app->bot_id >> 22) % 6);
    copy_bytes(url + prefix_bytes + 1, ".png", 4);
    return url_picture({url, prefix_bytes + 5}, MediaAttachmentFormat::png);
}
// Puts the app's picture on a picker row.
bool avatar(PopupItem& item, const CommandApplication* app) noexcept {
    const MediaAssetKey key = app_picture(app);
    if (key.kind == MediaAssetKind::none)
        return false;
    item.flags |= popup_item_avatar;
    item.media_id = key.id;
    const bool url = key.kind == MediaAssetKind::profile_image;
    item.media_variant = url ? static_cast<u32>(media_asset_embed_format(key)) : key.variant;
    item.media_flags = url ? kPopupItemMediaUrl : (key.flags & media_asset_flag_animated) ? 1 : 0;
    return true;
}
constexpr u32 no_option = ~0u;
// The selected command's options in definition order.
struct OptionList {
    JsonValue options[25];
    u32 count;
};
OptionList command_options() noexcept {
    OptionList list{};
    const auto& c = *commands;
    if (c.selected.command && c.selected.command->options.source.bytes)
        json_visit_indexed_array(
            c.selected.index->document, c.selected.command->options, 64,
            [](void* opaque, u32, JsonValue option) noexcept {
                auto& l = *static_cast<OptionList*>(opaque);
                if (l.count < 25)
                    l.options[l.count++] = option;
                return true;
            },
            &list);
    return list;
}
bool option_flag(JsonValue option, const char* key) noexcept {
    bool value = false;
    json_value_to_bool(command_member(commands->selected.index->document, option, {key, c_string_length(key)}),
                       &value);
    return value;
}
// Option type of one entry of a command's options array (0 when unreadable).
u64 option_type(const CommandIndex& index, JsonValue option) noexcept {
    u64 type = 0;
    json_value_to_u64(command_member(index.document, option, utf8_literal("type")), &type);
    return type;
}
// The selected command's typed options in the draft, in absolute bytes.
u32 typed_options(Utf8View input, Slice<CommandArgumentSpan> spans) noexcept {
    const auto& c = *commands;
    const u32 start = c.selected.command->name.bytes + 1;
    if (input.bytes <= start)
        return 0;
    const u32 count = command_argument_spans(*c.selected.index, *c.selected.command,
                                             {input.data + start, input.bytes - start}, spans);
    for (u32 n = 0; n < count; ++n) {
        spans[n].key_first += start;
        spans[n].value_first += start;
        spans[n].value_last += start;
        spans[n].end_last += start;
    }
    return count;
}
// The chip the caret is in (up to its end marker), or no_option: after the
// marker the caret is outside.
u32 chip_at(Utf8View, const CommandArgumentSpan* spans, u32 count, u32 caret) noexcept {
    for (u32 n = 0; n < count; ++n)
        if (caret >= spans[n].value_first && caret <= spans[n].value_last)
            return n;
    return no_option;
}
bool option_end_at(Utf8View input, u32 at) noexcept {
    return at + kCommandOptionEndBytes <= input.bytes &&
           bytes_equal(input.data + at, kCommandOptionEnd, kCommandOptionEndBytes);
}
// The word the caret ends outside every chip: back to a blank, an option's
// end or the command's name.
Utf8View caret_word(Utf8View input, u32 caret) noexcept {
    const u32 floor = commands->selected.command->name.bytes + 1;
    u32 first = minimum(caret, input.bytes);
    while (first > floor && !command_space(input.data[first - 1]) &&
           !(first >= kCommandOptionEndBytes && option_end_at(input, first - kCommandOptionEndBytes)))
        --first;
    return {input.data + first, minimum(caret, input.bytes) - first};
}
ConstSlice<CommandAcceptedChoice> accepted_choices() noexcept {
    return {commands->accepted, commands->accepted_count};
}
// Option checks after an edit: the option the caret
// left and options that failed and changed are checked again; validate_all
// checks every option (allow_empty while the command is being set up).
void check_options(const CommandArgumentSpan* spans, u32 count, u32 active_slot, bool validate_all,
                   bool allow_empty) noexcept {
    auto& c = *commands;
    const auto list = command_options();
    for (u32 slot = 0; slot < list.count; ++slot) {
        auto& state = c.option_states[slot];
        const CommandArgumentSpan* span = nullptr;
        for (u32 n = 0; n < count && !span; ++n)
            span = spans[n].slot == slot ? spans + n : nullptr;
        const Utf8View value = span ? span->value : Utf8View{};
        const u64 hash = span ? hash64_bytes(kHash64Seed, value.data, value.bytes) | 1 : 0;
        const bool check = validate_all || (slot == c.previous_slot && c.previous_slot != active_slot) ||
                           (state.error.bytes && hash != state.value_hash);
        state.value_hash = hash;
        if (check)
            state.error = command_option_check(*c.selected.index, list.options[slot], span != nullptr, value,
                                               allow_empty, {state.message, sizeof(state.message)},
                                               accepted_choices());
    }
    c.previous_slot = active_slot;
}
void reset_option_states(Commands& c) noexcept {
    for (auto& state : c.option_states) {
        reset_value(&state.error);
        state.value_hash = 0;
    }
    c.previous_slot = no_option;
    c.focused_slot = no_option;
    reset_value(&c.focused_option);
    c.accepted_count = 0;
    c.focused_revision = ~0u;
}
// The chip the caret is in becomes the active option; the option it left and
// failed options whose value changed are checked again. Once per
// composer revision: the composer's projection asks first, so the chips it
// draws show the checks of this very edit.
void update_focus(Utf8View input) noexcept {
    auto& c = *commands;
    if (!c.selected.command || c.focused_revision == g_app.message_composer.revision)
        return;
    c.focused_revision = g_app.message_composer.revision;
    CommandArgumentSpan spans[25];
    const u32 count = typed_options(input, {spans, 25});
    const u32 chip = chip_at(input, spans, count, g_app.message_composer.caret);
    reset_value(&c.focused_option);
    c.focused_slot = no_option;
    if (chip != no_option) {
        c.focused_option = spans[chip].option;
        c.focused_slot = spans[chip].slot;
        c.value_begin = spans[chip].value_first;
        c.value_end = spans[chip].value_last;
    }
    check_options(spans, count, c.focused_slot, false, false);
}
bool autocomplete_enabled() noexcept {
    const auto& c = *commands;
    return c.selected.command && c.focused_option.source.bytes && option_flag(c.focused_option, "autocomplete");
}
u64 next_nonce() noexcept {
    const u64 stamp = (clock_unix_milliseconds() - kSnowflakeEpochMilliseconds) << 22;
    auto& serial = commands->nonce_serial;
    serial = maximum(serial + 1, stamp);
    return serial;
}
// The focused option's text, used to query its choices.
Utf8View focused_query() noexcept {
    const auto& c = *commands;
    const auto input = message_composer_text(g_app.message_composer);
    if (!c.focused_option.source.bytes || c.value_begin > c.value_end || c.value_end > input.bytes)
        return {};
    u32 first = c.value_begin, last = c.value_end;
    while (first < last && command_space(input.data[first]))
        ++first;
    while (last > first && command_space(input.data[last - 1]))
        --last;
    return {input.data + first, last - first};
}
// A title row of the option lists (plain
// text, upper case): the title, or "<matching> <query>" with the query too.
void list_title(PopupItem* items, u32* count, Utf8View title, Utf8View matching, Utf8View query) noexcept {
    if (*count >= 31)
        return;
    auto& c = *commands;
    const u32 n = (*count)++;
    Utf8View label = title;
    if (query.bytes) {
        label = copy_label(c.labels[n], 512, matching, utf8_literal(" "), query);
        for (u32 at = matching.bytes; at < label.bytes; ++at)
            if (c.labels[n][at] >= 'a' && c.labels[n][at] <= 'z')
                c.labels[n][at] = static_cast<char>(c.labels[n][at] - ('a' - 'A'));
    }
    // Popup actions are unique: each title takes its own below the messages'.
    items[n] = {label, value_base - 2 - n, popup_item_command_section, 0};
}
// The focused option's choice list: BOOLEAN True/False, static choices ranked
// against the option's text, or
// the application's suggestions. Returns false when the option has no list.
bool render_choices(PopupItem* items, u32* count) noexcept {
    auto& c = *commands;
    c.choice_count = 0;
    c.choice_suggested = false;
    if (!c.focused_option.source.bytes)
        return false;
    const Utf8View query = focused_query();
    const auto& doc = c.selected.index->document;
    if (autocomplete_enabled()) {
        const auto& state = c.autocomplete;
        c.choice_suggested = true;
        if (state.phase == CommandAutocompletePhase::failed) {
            items[(*count)++] = {utf8_literal("Loading options failed"), value_base - 1, popup_item_none, 0};
            return true;
        }
        if (state.phase == CommandAutocompletePhase::ready && !state.suggestions.count) {
            items[(*count)++] = {utf8_literal("No options match your search"), value_base - 1, popup_item_none, 0};
            return true;
        }
        list_title(items, count, utf8_literal("OPTIONS"), utf8_literal("OPTIONS MATCHING"), query);
        // While the application answers, five loading rows show.
        if (state.phase != CommandAutocompletePhase::ready) {
            for (u32 seed = 0; seed < 5 && *count < 31; ++seed)
                items[(*count)++] = popup_placeholder_row(value_base - 40 - seed, seed);
            return true;
        }
        for (u32 slot = 0; slot < state.suggestions.count; ++slot) {
            const auto& choice = state.suggestions.choices[slot];
            c.choice_bytes[slot] = copy_label(c.choice_names[slot], 512, choice.name).bytes;
            ++c.choice_count;
            if (*count < 31)
                items[(*count)++] = {{c.choice_names[slot], c.choice_bytes[slot]}, value_base + slot,
                                     popup_item_enabled, 0};
        }
        return true;
    }
    // Static choices (or True/False): every name scoring above zero, best first.
    const auto choices = command_member(doc, c.focused_option, utf8_literal("choices"));
    const bool boolean = option_type(*c.selected.index, c.focused_option) == 5;
    if (!boolean && choices.kind != JsonValueKind::array)
        return false;
    struct Ranked {
        Commands* c;
        Utf8View query;
        u32 score[25];
    } ranked{&c, query, {}};
    const auto add = [](Ranked& r, Utf8View name) noexcept {
        const u32 score = command_choice_score(name, r.query);
        if (!score || r.c->choice_count == 25 || !name.bytes)
            return;
        // A stable insertion by descending score.
        u32 at = r.c->choice_count++;
        while (at && r.score[at - 1] < score) {
            r.score[at] = r.score[at - 1];
            copy_bytes(r.c->choice_names[at], r.c->choice_names[at - 1], r.c->choice_bytes[at - 1]);
            r.c->choice_bytes[at] = r.c->choice_bytes[at - 1];
            --at;
        }
        r.score[at] = score;
        r.c->choice_bytes[at] = copy_label(r.c->choice_names[at], 512, name).bytes;
    };
    if (boolean) {
        add(ranked, utf8_literal("True"));
        add(ranked, utf8_literal("False"));
    } else {
        struct Visit {
            Ranked* ranked;
            const decltype(add)* add;
        } visit{&ranked, &add};
        json_visit_indexed_array(
            doc, choices, 64,
            [](void* opaque, u32, JsonValue choice) noexcept {
                auto& v = *static_cast<Visit*>(opaque);
                char storage[512];
                (*v.add)(*v.ranked, field_text(*v.ranked->c->selected.index, choice, "name", {storage, 512}));
                return true;
            },
            &visit);
    }
    if (!c.choice_count) {
        items[(*count)++] = {utf8_literal("No options match your search"), value_base - 1, popup_item_none, 0};
        return true;
    }
    list_title(items, count, utf8_literal("OPTIONS"), utf8_literal("OPTIONS MATCHING"), query);
    for (u32 slot = 0; slot < c.choice_count && *count < 31; ++slot)
        items[(*count)++] = {{c.choice_names[slot], c.choice_bytes[slot]}, value_base + slot,
                             popup_item_enabled, 0};
    return true;
}
// The option list outside every chip: the options without
// a value whose name starts with the word at the caret, required ones first
// under their own title.
void render_options(PopupItem* items, u32* count, const CommandArgumentSpan* spans, u32 typed,
                    Utf8View query) noexcept {
    auto& c = *commands;
    const auto list = command_options();
    for (u32 pass = 0; pass < 2; ++pass) {
        bool titled = false;
        for (u32 slot = 0; slot < list.count && *count < 31; ++slot) {
            bool present = false;
            for (u32 n = 0; n < typed && !present; ++n)
                present = spans[n].slot == slot;
            char storage[128];
            const auto name = field_text(*c.selected.index, list.options[slot], "name", {storage, sizeof(storage)});
            if (present || option_flag(list.options[slot], "required") != (pass == 0) || name.bytes < query.bytes ||
                !utf8_equal({name.data, query.bytes}, query))
                continue;
            if (!titled) {
                titled = true;
                list_title(items, count,
                           pass == 0        ? utf8_literal("REQUIRED OPTIONS")
                           : c.option_count ? utf8_literal("OTHER OPTIONS")
                                            : utf8_literal("OPTIONS"),
                           pass == 0 ? utf8_literal("REQUIRED OPTIONS MATCHING") : utf8_literal("OPTIONS MATCHING"),
                           query);
            }
            const u32 n = (*count)++;
            items[n] = {copy_label(c.labels[n], 512, name), option_base + slot,
                        popup_item_enabled, 0};
            items[n].detail = field_text(*c.selected.index, list.options[slot], "description", {c.details[n], 1024});
            ++c.option_count;
        }
    }
}
// Closes the commands popup without dismissing the draft's completion (a
// list with no rows; the next one opens again).
void hide_popup() noexcept {
    auto& c = *commands;
    const u64 dismissed_document = c.dismissed_document;
    const u32 dismissed_revision = c.dismissed_revision;
    close_popup();
    c.dismissed_document = dismissed_document;
    c.dismissed_revision = dismissed_revision;
}
float list_item_height(const PopupItem& item) noexcept {
    return (item.flags & popup_item_command_section) ? kCommandTitleHeight
           : popup_item_placeholder(item)            ? kCommandGenericLoadingHeight
                                                     : kCommandGenericRowHeight;
}
// The selected command's list under the composer's option bar: the
// active option's choices, the unset options matching the word at the caret
// while it is outside every chip, or the file picker on an ATTACHMENT chip.
bool render_command(const UiLayoutNode& surface) noexcept {
    auto& c = *commands;
    PopupItem items[32]{};
    u32 count = 0;
    const auto input = message_composer_text(g_app.message_composer);
    CommandArgumentSpan spans[25];
    const u32 typed = typed_options(input, {spans, 25});
    c.option_count = 0;
    c.choices_shown = render_choices(items, &count);
    if (!c.choices_shown && !c.focused_option.source.bytes) {
        render_options(items, &count, spans, typed, caret_word(input, g_app.message_composer.caret));
    } else if (!c.choices_shown && option_type(*c.selected.index, c.focused_option) == 11) {
        // An ATTACHMENT chip offers the file picker.
        list_title(items, &count, utf8_literal("OPTIONS"), {}, {});
        items[count++] = {utf8_literal("Drag and drop or click to upload file"), option_base + c.focused_slot,
                          popup_item_enabled, 0};
        c.option_count = 1;
    }
    if (c.choices_shown)
        c.option_count = c.choice_count;
    // A lone message ("No options match your search") takes the title row.
    if (count == 1 && !(items[0].flags & popup_item_command_section))
        items[0].flags = popup_item_command_section;
    c.list_height = 0;
    for (u32 n = 0; n < count; ++n)
        c.list_height += list_item_height(items[n]) +
                         (n && (items[n].flags & popup_item_command_section) ? kCommandSectionGap : 0.f);
    c.rendered_generation = c.generation;
    // /gif and /sticker show their results above the composer for the query option.
    Utf8View query{};
    for (u32 n = 0; n < typed; ++n)
        if (utf8_equal(spans[n].name, utf8_literal("query")))
            query = spans[n].value;
    // Without rows there is no list, only the option bar.
    if (!count) {
        hide_popup();
        return builtin_command_results(c.selected.command, query, surface.rectangle);
    }
    const UiRect viewport = application_viewport_dips();
    const UiRect anchor = surface.rectangle;
    const auto result = popup_scene_open(&g_app.popup_scene, PopupKind::autocomplete, c.channel, {items, count},
                                         anchor, viewport, ui_rect_width(anchor), true, c.offset, 0);
    if (!result.ok())
        return false;
    set_popup_owner(PopupOwnerKind::commands);
    g_app.popup_scene.selected_index = ~0u;
    // The keyboard row counts the list's actionable rows from one.
    if (c.keyboard_row != no_row && c.option_count) {
        c.keyboard_row = minimum(maximum(c.keyboard_row, 1u), c.option_count);
        for (u32 n = 0, row = 0; n < count; ++n)
            if ((items[n].flags & popup_item_enabled) && ++row == c.keyboard_row) {
                g_app.popup_scene.selected_index = n;
                break;
            }
    }
    return builtin_command_results(c.selected.command, query, g_app.popup_scene.rectangle);
}
// The usage summary after a command name: its required option names
// (drawn as chips), then "+N optional" for the rest; parts are \x1E-separated.
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
                if (name.bytes && s.writer.used + name.bytes + 1 <= s.writer.output.count) {
                    if (s.writer.used)
                        protocol_writer_append_byte(&s.writer, 0x1E);
                    protocol_writer_append_bytes(&s.writer, {reinterpret_cast<const u8*>(name.data), name.bytes});
                }
                return true;
            },
            &summary);
    if (summary.optional && summary.writer.used + 24 <= output.count) {
        if (summary.writer.used)
            protocol_writer_append_byte(&summary.writer, 0x1E);
        protocol_writer_append_byte(&summary.writer, '+');
        protocol_writer_append_u64(&summary.writer, summary.optional);
        protocol_writer_append_literal(&summary.writer, " optional");
    }
    return {output.data, summary.writer.used};
}
// Picker geometry, virtual from the top of each column (see kCommandPicker*).
// The row before Built-in is 8 DIPs taller; the
// divider line in that gap is drawn unless Frequently Used is that row.
u32 rail_divider(const Commands& c) noexcept {
    return c.rail_count >= 2 && c.rail[c.rail_count - 1].kind == SectionKind::builtin ? c.rail_count - 1 : no_row;
}
float rail_top(const Commands& c, u32 r) noexcept {
    const u32 divider = rail_divider(c);
    return kCommandRailLead + static_cast<float>(r) * kCommandRailStride +
           (divider != no_row && r >= divider ? kCommandRailDivider : 0.f);
}
float rail_extent(const Commands& c) noexcept {
    return c.rail_count ? rail_top(c, c.rail_count - 1) + kCommandRailIcon + kCommandListInset : 0.f;
}
// A section's rows: its commands and, while indexes load, the
// placeholder rows after them.
u32 section_rows(const Section& section) noexcept {
    return section.count + section.placeholders;
}
float section_top(const Commands& c, u32 s) noexcept {
    float y = kCommandListInset;
    for (u32 n = 0; n < s && n < c.body_count; ++n)
        y += kCommandSectionHeight + static_cast<float>(section_rows(c.body[n])) * kCommandRowHeight +
             kCommandSectionGap;
    return y;
}
// Discovery without sections shows one status row; query results add their
// title and the list's bottom padding.
float body_extent(const Commands& c) noexcept {
    if (!c.discovery)
        return kCommandTitleHeight +
               static_cast<float>(c.plugin_count + maximum(c.count + c.query_placeholders, 1u)) * kCommandRowHeight +
               kCommandListInset;
    return c.body_count && !c.error.bytes ? section_top(c, c.body_count) - kCommandSectionGap
                                          : kCommandListInset + kCommandRowHeight;
}
// Virtual top of the row of entry k.
float row_top(const Commands& c, u32 k) noexcept {
    if (!c.discovery)
        return kCommandTitleHeight + static_cast<float>(c.plugin_count + k) * kCommandRowHeight;
    for (u32 s = 0; s < c.body_count; ++s) {
        const auto& section = c.body[s];
        if (section.kind != SectionKind::plugins && k >= section.first && k < section.first + section.count)
            return section_top(c, s) + kCommandSectionHeight + static_cast<float>(k - section.first) * kCommandRowHeight;
    }
    return 0;
}
// The rail entry highlighted: the filtered section, else the one
// the list is scrolled to (its top within 20 DIPs of the view's top).
u32 active_rail(const Commands& c) noexcept {
    if (!c.discovery || !c.rail_count)
        return no_row;
    for (u32 r = 0; c.filter_app && r < c.rail_count; ++r)
        if (c.rail[r].app && c.rail[r].app->id == c.filter_app)
            return r;
    u32 spy = 0;
    for (u32 s = 1; s < c.body_count; ++s)
        if (section_top(c, s) <= c.offset + spy_lead)
            spy = s;
    // Local plugin commands lead the body but have no rail entry.
    const u32 lead = c.body_count && c.body[0].kind == SectionKind::plugins ? 1u : 0u;
    return spy >= lead ? minimum(spy - lead, c.rail_count - 1) : 0;
}
Utf8View section_label(const Section& section) noexcept {
    return section.kind == SectionKind::plugins    ? utf8_literal("Plugins")
           : section.kind == SectionKind::frequent ? utf8_literal("Frequently Used")
                                                   : section_name(*section.app);
}
// The section's rail and header picture: the clock, Built-in's slash, the
// app's bot (a generic app glyph without one).
void section_picture(PopupItem& item, const Section& section) noexcept {
    if (section.kind == SectionKind::app && avatar(item, section.app))
        return;
    item.presentation = ui_menu_row_presentation(section.kind == SectionKind::frequent  ? UiIcon::recent
                                                 : section.kind == SectionKind::builtin ? UiIcon::command
                                                                                        : UiIcon::bot);
}
// The row's right column: the bot's server nickname, else the section name.
Utf8View row_source(const CommandApplication& app) noexcept {
    const auto& c = *commands;
    if (c.guild && app.bot_id)
        if (const auto* member = guild_member_store_find(g_app.account_ui->guild_member_store, c.guild, app.bot_id))
            if (const auto nick = guild_member_store_nickname(*member); nick.bytes)
                return nick;
    return section_name(app);
}
// One command row: "/name", then description, source and the usage summary
// (\x1F-separated); query results and Frequently Used show the app picture.
void command_row(PopupItem& item, u32 slot, u32 entry_index, bool picture) noexcept {
    auto& c = *commands;
    const auto& entry = c.entries[entry_index];
    item = {copy_label(c.labels[slot], 512, utf8_literal("/"), entry.command->name), choose_base + entry_index,
            popup_item_enabled | popup_item_command_row, 0};
    char summary[512];
    const Utf8View options = option_summary(*entry.index, *entry.command, {summary, sizeof(summary)});
    const u32 used = copy_label(c.details[slot], 1024, entry.command->description, utf8_literal("\x1F"),
                                row_source(*entry.app))
                         .bytes;
    item.detail = {c.details[slot],
                   used + copy_label(c.details[slot] + used, 1024 - used, utf8_literal("\x1F"), options).bytes};
    if (picture && entry.app->id != kBuiltinCommandApplication && !avatar(item, entry.app))
        item.presentation = ui_menu_row_presentation(UiIcon::bot);
    else if (picture && entry.app->id == kBuiltinCommandApplication)
        item.presentation = ui_menu_row_presentation(UiIcon::command);
}
void plugin_row(PopupItem& item, u32 slot, u32 n, const NativePluginSdkCommandView& local) noexcept {
    auto& c = *commands;
    item = {copy_label(c.labels[slot], 512, utf8_literal("/"), local.name), plugin_base + n,
            popup_item_enabled | popup_item_command_row, 0};
    item.detail = copy_label(c.details[slot], 1024, local.description, utf8_literal("\x1F"), utf8_literal("Plugins"));
}
constexpr u32 section_base = 600000, pinned_base = 700000, placeholder_base = 800000;
// The unselected picker: discovery (rail, then sections from the scroll
// offset with the current section's header pinned) or query results (title
// and every ranked row, scrolled as a whole).
bool render_picker(const UiLayoutNode& surface) noexcept {
    auto& c = *commands;
    PopupItem items[32]{};
    u32 count = 0;
    float fraction = 0, rail_fraction = 0;
    NativePluginSdkCommandView local[plugin_rows];
    const auto input = message_composer_text(g_app.message_composer);
    const u32 local_count =
        input.bytes ? application_plugins_commands(command_query({input.data + 1, input.bytes - 1}),
                                                   {local, plugin_rows})
                    : 0;
    const Utf8View status = c.error.bytes ? c.error : utf8_literal("No matching commands");
    if (c.discovery) {
        const u32 active = active_rail(c), divider = rail_divider(c);
        u32 first = 0;
        while (first + 1 < c.rail_count && rail_top(c, first + 1) <= c.rail_offset)
            ++first;
        rail_fraction = c.rail_count ? c.rail_offset - (rail_top(c, first) - kCommandRailLead) : 0.f;
        for (u32 r = first; r < c.rail_count && count < 12 && rail_top(c, r) < c.rail_offset + kCommandPickerHeight;
             ++r) {
            auto& item = items[count];
            item = {section_label(c.rail[r]), rail_base + r, popup_item_enabled | popup_item_command_category, 0};
            if (r == active)
                item.flags |= popup_item_checked;
            if (r == divider)
                item.flags |= popup_item_separator_before;
            section_picture(item, c.rail[r]);
            ++count;
        }
        if (!count) {
            // Built-in is always listed; this only guards an empty catalog.
            items[count++] = {utf8_literal("Built-in"), rail_base, popup_item_command_category, 0};
            items[0].presentation = ui_menu_row_presentation(UiIcon::command);
        }
    } else {
        if (!c.count && !local_count && !c.query_placeholders && !c.error.bytes) {
            // Command results that match nothing are hidden.
            close_popup();
            c.rendered_generation = c.generation;
            return true;
        }
        // The title is one plain line, the query uppercased too.
        const Utf8View title = copy_label(c.labels[count], 512, utf8_literal("COMMANDS MATCHING /"),
                                          {c.query, c.query_bytes});
        for (u32 at = 0; at < title.bytes; ++at)
            if (c.labels[count][at] >= 'a' && c.labels[count][at] <= 'z')
                c.labels[count][at] = static_cast<char>(c.labels[count][at] - ('a' - 'A'));
        items[count] = {title, section_base - 1, popup_item_command_section, 0};
        ++count;
    }
    const u32 body_begin = count;
    if (c.error.bytes || (!c.count && !local_count && !c.query_placeholders && !c.discovery)) {
        items[count++] = {status, choose_base - 1, popup_item_command_row, 0};
        fraction = 0;
    } else if (!c.discovery) {
        for (u32 n = 0; n < local_count && count < 31; ++n, ++count)
            plugin_row(items[count], count, n, local[n]);
        for (u32 n = 0; n < c.count && count < 31; ++n, ++count)
            command_row(items[count], count, n, true);
        // Three loading rows follow the results.
        for (u32 n = 0; n < c.query_placeholders && count < 32; ++n, ++count)
            items[count] = popup_placeholder_row(placeholder_base + n, n, popup_item_command_row);
        fraction = minimum(c.offset, maximum(0.f, body_extent(c) - kCommandRowHeight));
    } else {
        // The section under the offset keeps its header pinned while any of
        // its rows remain in view.
        u32 pinned = no_row;
        for (u32 s = 0; s < c.body_count; ++s) {
            const float top = section_top(c, s);
            const float rows_end =
                top + kCommandSectionHeight + static_cast<float>(section_rows(c.body[s])) * kCommandRowHeight;
            if (c.offset > top && c.offset < rows_end)
                pinned = s;
        }
        bool started = false;
        const float bottom = c.offset + kCommandPickerHeight;
        for (u32 s = 0; s < c.body_count && count < 31; ++s) {
            const auto& section = c.body[s];
            const float top = section_top(c, s);
            if (top >= bottom)
                break;
            const float gap = s + 1 < c.body_count ? kCommandSectionGap : 0.f;
            // Each item with the gap after it; the first one past the offset starts the flow.
            const auto start = [&](float item_top, float item_end) noexcept {
                if (!started && item_end > c.offset) {
                    started = true;
                    fraction = c.offset - (item_top - kCommandListInset);
                }
                return started;
            };
            const u32 rows = section_rows(section);
            const float header_end = top + kCommandSectionHeight + (rows ? 0.f : gap);
            if (start(top, header_end)) {
                auto& item = items[count];
                item = {section_label(section), section_base + s, popup_item_command_section, 0};
                section_picture(item, section);
                ++count;
            }
            for (u32 k = 0; k < rows && count < 31; ++k) {
                const float row = top + kCommandSectionHeight + static_cast<float>(k) * kCommandRowHeight;
                if (row >= bottom)
                    break;
                if (!start(row, row + kCommandRowHeight + (k + 1 == rows ? gap : 0.f)))
                    continue;
                if (k >= section.count)
                    items[count] = popup_placeholder_row(placeholder_base + k, k, popup_item_command_row);
                else if (section.kind != SectionKind::plugins)
                    command_row(items[count], count, section.first + k, section.kind == SectionKind::frequent);
                else if (k < local_count)
                    plugin_row(items[count], count, k, local[k]);
                else
                    continue;
                ++count;
            }
        }
        if (pinned != no_row && count < 32) {
            auto& item = items[count];
            item = {section_label(c.body[pinned]), pinned_base + pinned,
                    popup_item_command_section | popup_item_command_pinned, 0};
            section_picture(item, c.body[pinned]);
            ++count;
        }
    }
    const UiRect viewport = application_viewport_dips();
    const UiRect anchor = surface.rectangle;
    const auto result = popup_scene_open(&g_app.popup_scene, PopupKind::autocomplete, c.channel, {items, count},
                                         anchor, viewport, ui_rect_width(anchor), true, fraction, rail_fraction);
    if (!result.ok())
        return false;
    set_popup_owner(PopupOwnerKind::commands);
    g_app.popup_scene.selected_index = ~0u;
    // The thin scrollbar shows where the virtual list stands.
    g_app.popup_scene.picker_scroll_offset = c.offset;
    g_app.popup_scene.picker_scroll_extent = body_extent(c);
    if (c.rail_focus != no_row) {
        for (u32 n = 0; n < body_begin; ++n)
            if (items[n].action == rail_base + c.rail_focus)
                g_app.popup_scene.selected_index = n;
    } else if (c.keyboard_row != no_row && c.count) {
        c.keyboard_row = minimum(c.keyboard_row, c.count - 1);
        for (u32 n = body_begin; n < count; ++n)
            if (items[n].action == choose_base + c.keyboard_row && (items[n].flags & popup_item_enabled)) {
                g_app.popup_scene.selected_index = n;
                break;
            }
    }
    c.rendered_generation = c.generation;
    return true;
}
bool render() noexcept {
    auto& c = *commands;
    const auto* surface = ui_widget_manager_layout_node(g_app.message_composer_scene.widgets,
                                                         g_app.message_composer_scene.surface);
    if (!surface)
        return true;
    return c.selected.command ? render_command(*surface) : render_picker(*surface);
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
    return false;
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
Utf8View trimmed_text(Utf8View input, u32 first, u32 last) noexcept {
    while (first < last && command_space(input.data[first]))
        ++first;
    while (last > first && command_space(input.data[last - 1]))
        --last;
    return {input.data + first, last - first};
}
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
    if (matches != 1)
        return false;
    c.selected = found;
    c.offset = 0;
    return true;
}
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
} // namespace
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
    // description, its failed check instead in the danger colour; else the
    // command's "/name" and description, or the command's error.
    projection->command_bar_name = copy_label(commands->bar_name, sizeof(commands->bar_name), utf8_literal("/"), name);
    projection->command_bar_detail = c.selected.command->description;
    projection->command_bar_error = false;
    if (prepared && c.focused_option.source.bytes) {
        const auto& state = c.option_states[c.focused_slot];
        projection->command_bar_name =
            field_text(*c.selected.index, c.focused_option, "name", {commands->bar_name, sizeof(commands->bar_name)});
        projection->command_bar_detail =
            state.error.bytes ? state.error
                              : field_text(*c.selected.index, c.focused_option, "description",
                                           {commands->bar_detail, sizeof(commands->bar_detail)});
        projection->command_bar_error = state.error.bytes != 0;
    } else if (c.error.bytes) {
        projection->command_bar_detail = c.error;
        projection->command_bar_error = true;
    }
    if (c.selected.app)
        projection->command.avatar = app_picture(c.selected.app);
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
            (input.bytes > name.bytes + 1 && !command_space(input.data[name.bytes + 1])))
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
        const bool before = first && !command_space(input.data[first - 1]) &&
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
    if (c.selected.command->application_id == kBuiltinCommandApplication) {
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
        const auto run = builtin_command_run(*c.selected.command, {options, writer.used}, c.channel,
                                             {text_storage, sizeof(text_storage)}, &text);
        if (run == BuiltinCommandRun::failed) {
            c.error = utf8_literal("This command couldn't run. Try again.");
            return reopen();
        }
        // /gif and /sticker send only a chosen result; the draft stays and the app shakes.
        if (run == BuiltinCommandRun::rejected) {
            shake_app();
            return reopen();
        }
        // A built-in's use is recorded as it runs.
        frecency_track(*c.selected.command, c.guild);
        builtin_command_results(nullptr, {}, {});
        command_autocomplete_cancel(&c.autocomplete);
        reset_value(&c.error);
        reset_value(&c.selected);
        close_popup();
        if (run == BuiltinCommandRun::handled)
            return composer_apply_result(message_composer_clear(&g_app.message_composer), true);
        // A text command's result is sent as the message itself.
        const auto replaced = message_composer_replace_range(&g_app.message_composer, 0,
                                                             g_app.message_composer.bytes, text);
        if (replaced == MessageComposerResult::invalid_input)
            return true;
        c.builtin_text = true;
        c.builtin_text_revision = g_app.message_composer.text_revision;
        *handled = false;
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
    auto rect = g_app.popup_scene.rectangle;
    if (c.selected.command) {
        if (rail || c.list_height <= 0.f)
            return false;
        *range = {rect, c.offset,
                  maximum(0.0, static_cast<double>(c.list_height + kCommandListInset - ui_rect_height(rect))),
                  kCommandGenericRowHeight, 1, 0, c.generation};
        return true;
    }
    // The picker's columns run to the panel edges; query results have no rail.
    if (rail && !c.discovery)
        return false;
    if (c.discovery) {
        if (rail)
            rect.right = minimum(rect.right, rect.left + kCommandRailWidth);
        else
            rect.left = minimum(rect.right, rect.left + kCommandRailWidth);
    }
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
    if (key == VK_UP || key == VK_DOWN || key == VK_HOME || key == VK_END) {
        c.rail_focus = key == VK_HOME ? 0
                       : key == VK_END ? c.rail_count - 1
                       : key == VK_UP  ? (r ? r - 1 : 0)
                                       : minimum(r + 1, c.rail_count - 1);
        return reveal_rail(c, c.rail_focus) && render();
    }
    if (key == VK_RETURN || key == VK_SPACE) {
        // The key's character must not reach the draft.
        g_app.composer_menu_held_key = key;
        if (!application_commands_activate(rail_base + r))
            return false;
        c.rail_focus = minimum(r, c.rail_count ? c.rail_count - 1 : 0);
        return reveal_rail(c, c.rail_focus) && render();
    }
    if (key == VK_RIGHT || key == VK_TAB || key == VK_ESCAPE) {
        c.rail_focus = no_row;
        // Into the list at the section the rail entry names (Escape keeps the row).
        if (key != VK_ESCAPE && r < c.rail_count) {
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
    return key == VK_LEFT;
}
bool picker_key(Commands& c, u32 key) noexcept {
    if (key == VK_RETURN && application_plugins_command_pending()) {
        close_popup();
        return queue_message_send();
    }
    if (c.rail_focus != no_row && c.rail_focus < c.rail_count)
        return rail_key(c, key);
    if (key == VK_LEFT && c.discovery && c.rail_count) {
        c.rail_focus = minimum(active_rail(c), c.rail_count - 1);
        return reveal_rail(c, c.rail_focus) && render();
    }
    if (key == VK_RETURN || key == VK_TAB) {
        if (c.count && c.keyboard_row < c.count)
            return application_commands_activate(choose_base + c.keyboard_row);
        // Without a keyboard selection Tab first selects the discovery list's
        // top row; otherwise the key keeps its composer meaning.
        if (key == VK_TAB && c.discovery && c.count) {
            c.keyboard_row = 0;
            return reveal_row(c, 0) && render();
        }
        return false;
    }
    if (key == VK_UP || key == VK_DOWN) {
        if (!c.count)
            return true;
        const u32 last = c.count - 1;
        // Discovery stops at its ends; query results wrap.
        if (c.keyboard_row > last)
            c.keyboard_row = key == VK_UP && !c.discovery ? last : 0;
        else if (key == VK_UP)
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
    if (!builtin_command_results_key(key, &handled))
        return false;
    if (handled)
        return true;
    if (key == VK_ESCAPE)
        return false;
    const u32 count = c.option_count;
    const bool row = c.keyboard_row != no_row && c.keyboard_row >= 1 && c.keyboard_row <= count;
    if (key == VK_RETURN || key == VK_TAB) {
        // A selected row is taken; else Tab moves to the next option and Enter
        // sends.
        if (const u32 action = row ? popup_scene_selected_action(g_app.popup_scene) : 0)
            return application_commands_activate(action);
        if (key == VK_TAB)
            return application_commands_option_tab(false);
        bool sent;
        // A built-in text command leaves its message for the ordinary send.
        return application_commands_send(&sent) && (sent || queue_message_send());
    }
    if (key == VK_UP || key == VK_DOWN) {
        if (!count)
            return true;
        if (!row)
            c.keyboard_row = key == VK_UP ? count : 1;
        else if (key == VK_UP)
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
    const u32 current = chip_at(input, spans, count, caret);
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
    c.rail = c.body = nullptr;
    c.count = c.rail_count = c.body_count = 0;
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
    shake_step(now);
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
