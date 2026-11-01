#include "commands_internal.hpp"

namespace maicord::app_detail {
namespace commands_detail {

Commands* commands;
Commands* ensure_commands() noexcept {
    if (!commands)
        commands = static_cast<Commands*>(
            VirtualAlloc(nullptr, sizeof(Commands), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return commands;
}

Frecency* frecency;

} // namespace commands_detail
namespace {

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

} // namespace
namespace commands_detail {

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

} // namespace commands_detail
namespace {

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

} // namespace
namespace commands_detail {

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
Utf8View copy_label(char* out, u32 capacity, Utf8View a, Utf8View b, Utf8View c,
                    Utf8View d) noexcept {
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

} // namespace commands_detail
} // namespace maicord::app_detail
