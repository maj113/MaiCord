#pragma once
#include "maicord/core/arena.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
struct CommandApplication {
    u64 id, bot_id;
    Utf8View name, icon, avatar, bot_name;
};
struct ApplicationCommand {
    u64 id, application_id, version, guild_id;
    Utf8View name, root_name, group_name, sub_name, description;
    JsonValue original, options;
    bool dm_permission, nsfw;
};
struct CommandIndex {
    Arena storage;
    JsonIndexedDocument document;
    CommandApplication* applications;
    ApplicationCommand* commands;
    u32 application_count, command_count;
    // User (type 2) and message (type 3) context menu commands, in index order.
    ApplicationCommand* context_commands;
    u8* context_types;
    u32 context_command_count;
};
inline constexpr u8 kCommandTypeUser = 2;
inline constexpr u8 kCommandTypeMessage = 3;
JsonValue command_member(const JsonIndexedDocument&, JsonValue, Utf8View name) noexcept;
bool command_json_text(JsonValue, Slice<char>, Utf8View*) noexcept;
bool command_index_decode(Utf8View, Slice<u32> scratch, CommandIndex*) noexcept;
void command_index_release(CommandIndex*) noexcept;
// The options JSON is produced by the typed composer; the original command
// metadata stays intact, including its version and root/subcommand structure.
// attachments_json, when present, is the staged uploads array (data.attachments).
bool command_data(const ApplicationCommand&, Utf8View options_json, ProtocolWriter*,
                  Utf8View attachments_json = {}) noexcept;
// Context menu invocation data: {"version","id","guild_id"?,"name","type",
// "options":[],"application_command","target_id"}.
bool command_context_data(const ApplicationCommand&, u8 type, u64 target_id, ProtocolWriter*) noexcept;
// Which staged uploads ATTACHMENT options take, in data.attachments order: the
// option value is the entry's index.
struct CommandAttachmentPlan {
    u8 file[10];
    u32 count;
};
// Parses named composer arguments (name:value, with optional JSON-quoted strings).
// Values retain their declared wire type; unknown/duplicate/missing arguments fail.
// ATTACHMENT options take the channel's staged uploads (files: their names in
// order): the one whose name the value gives, else the next unused one; a
// required one is filled even when not typed. Without a plan they are invalid.
bool command_arguments(const CommandIndex&, const ApplicationCommand&, Utf8View arguments, ProtocolWriter*,
                       Utf8View* error, Utf8View focused_name = {}, Utf8View focused_query = {},
                       ConstSlice<Utf8View> files = {}, CommandAttachmentPlan* plan = nullptr) noexcept;
// An application command interaction the API refused: the message the official
// client shows (the first nested _errors entry of an Invalid Form Body, else the
// top-level message) and whether the index must be refetched (stale command
// version, unknown command or unknown integration).
struct CommandInteractionFailure {
    Utf8View message;
    u64 code;
    bool stale_index;
};
bool command_interaction_failure(Utf8View body, Slice<u32> scratch, Slice<char> message,
                                 CommandInteractionFailure*) noexcept;
struct CommandArgumentFocus {
    JsonValue option;
    Utf8View name, value;
    u32 value_begin, value_end;
    bool quoted, closed;
};
// Byte ranges borrow the input. Tolerates a partially typed final argument.
bool command_argument_focus(const CommandIndex&, const ApplicationCommand&, Utf8View arguments, u32 caret,
                            CommandArgumentFocus*) noexcept;
bool command_argument_find(const CommandIndex&, const ApplicationCommand&, Utf8View arguments,
                           Utf8View name, CommandArgumentFocus*) noexcept;
bool command_argument_query(const CommandArgumentFocus&, Slice<char> scratch, Utf8View*) noexcept;
// Byte ranges of the typed option keys ("name:") of a partly typed argument
// list, in input order, for the composer's option pills. Returns the count.
struct CommandArgumentKey {
    u32 first, last;
};
u32 command_argument_keys(const CommandIndex&, const ApplicationCommand&, Utf8View arguments,
                          Slice<CommandArgumentKey> output) noexcept;
struct CommandChoice {
    Utf8View name, value;
    JsonValueKind kind;
};
struct CommandSuggestions {
    Arena storage;
    CommandChoice choices[25];
    u64 nonce;
    u32 count;
};
bool command_suggestions_decode(Utf8View body, Slice<u32> scratch, u64 expected_nonce,
                                CommandSuggestions*) noexcept;
void command_suggestions_release(CommandSuggestions*) noexcept;

// Command frecency inside the FRECENCY_AND_FAVORITES settings proto
// (settings-proto/2): application_command_frecency = 7, a map<string,
// FrecencyItem> application_commands = 1 keyed like the official client: the
// command id, "\0group" and "\0sub" for subcommands, and ":<guild>" for a
// server's own commands used there. FrecencyItem: total_uses = 1, recent_uses
// = 2 (packed unix ms), frecency = 3, score = 4.
inline constexpr u32 kCommandFrecencyMaximumEntries = 500;
inline constexpr u32 kCommandFrecencyKeyBytes = 128;
inline constexpr u32 kCommandFrecencyRecentUses = 10;
struct CommandFrecencyEntry {
    u64 recent_uses[kCommandFrecencyRecentUses];
    u32 total_uses;
    i32 frecency, score;
    u8 recent_count, key_bytes;
    char key[kCommandFrecencyKeyBytes];
};
// A settings-proto/2 GET or PATCH response: the entries (last duplicate key
// wins; longer keys are left out) and the data version. scratch holds the
// extracted groups (64 KiB suffices for 500 entries).
bool command_frecency_decode_response(Utf8View body, Slice<u8> scratch, Slice<CommandFrecencyEntry> entries,
                                      u32* count, u32* data_version) noexcept;
// The client's frecency at now: each recent use weighs 100/70/50/30/10/1 by
// age in whole days (up to 3/15/30/45/80/older) times 1%; frecency is
// ceil(total_uses * score / recent uses). Entries without a score are dropped;
// score is kept in hundredths. Returns the remaining count.
u32 command_frecency_compute(Slice<CommandFrecencyEntry> entries, u32 count, u64 now_unix_ms) noexcept;
// Records one use (only the latest ten are kept); a new key replaces the least
// frecent entry when the table is full.
void command_frecency_track(Slice<CommandFrecencyEntry> entries, u32* count, Utf8View key, u64 now_unix_ms) noexcept;
// PATCH body {"settings":"<base64>","required_data_version":N} carrying only
// application_command_frecency. Zero on capacity failure.
u32 command_frecency_write_patch(ConstSlice<CommandFrecencyEntry> entries, u32 data_version, Slice<u8> scratch,
                                 Slice<u8> json) noexcept;
// Accepts only such a body (root field 7 alone).
bool command_frecency_patch_valid(Utf8View json) noexcept;
} // namespace maicord
