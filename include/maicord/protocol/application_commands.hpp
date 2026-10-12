#pragma once
#include "maicord/core/arena.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
struct CommandApplication {
    u64 id, bot_id;
    Utf8View name, icon, avatar;
};
struct ApplicationCommand {
    u64 id, application_id, version, guild_id;
    Utf8View name, root_name, group_name, sub_name, description;
    JsonValue original, options;
    u64 default_member_permissions;
    bool has_member_permissions, dm_permission, nsfw;
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
bool command_data(const ApplicationCommand&, Utf8View options_json, ProtocolWriter*) noexcept;
// Context menu invocation data: {"version","id","guild_id"?,"name","type",
// "options":[],"application_command","target_id"} as the official client sends.
bool command_context_data(const ApplicationCommand&, u8 type, u64 target_id, ProtocolWriter*) noexcept;
// Parses named composer arguments (name:value, with optional JSON-quoted strings).
// Values retain their declared wire type; unknown/duplicate/missing arguments fail.
bool command_arguments(const CommandIndex&, const ApplicationCommand&, Utf8View arguments, ProtocolWriter*,
                       Utf8View* error, Utf8View focused_name = {}, Utf8View focused_query = {}) noexcept;
struct CommandArgumentFocus {
    JsonValue option;
    Utf8View name, value;
    u32 name_begin, value_begin, value_end;
    bool quoted, closed;
};
// Byte ranges borrow the input. Tolerates a partially typed final argument.
bool command_argument_focus(const CommandIndex&, const ApplicationCommand&, Utf8View arguments, u32 caret,
                            CommandArgumentFocus*) noexcept;
bool command_argument_find(const CommandIndex&, const ApplicationCommand&, Utf8View arguments,
                           Utf8View name, CommandArgumentFocus*) noexcept;
bool command_argument_query(const CommandArgumentFocus&, Slice<char> scratch, Utf8View*) noexcept;
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
} // namespace maicord
