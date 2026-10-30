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
// A choice the user took from an option's autocomplete suggestions: its name
// stands in the draft, its value is sent (the client's last choices per option).
struct CommandAcceptedChoice {
    Utf8View option, name, value;
    JsonValueKind kind;
};
// Builds the options array from the composer's arguments (command_argument_spans):
// each typed value, checked like the client (command_option_check), with its
// declared wire type in definition order. A failing option sets *error to the
// client's message. ATTACHMENT options take the channel's staged uploads (files:
// their names in order): the one whose name the value gives, else the next unused
// one; a required one is filled even when not typed. Without a plan they are invalid.
// focused_name marks an autocomplete request: that option sends focused_query and
// options that do not check out are left out instead of failing.
bool command_arguments(const CommandIndex&, const ApplicationCommand&, Utf8View arguments, ProtocolWriter*,
                       Utf8View* error, Utf8View focused_name = {}, Utf8View focused_query = {},
                       ConstSlice<Utf8View> files = {}, CommandAttachmentPlan* plan = nullptr,
                       ConstSlice<CommandAcceptedChoice> accepted = {}) noexcept;
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
// The end of an option chip in the composer's draft: U+2063 INVISIBLE
// SEPARATOR after a value marks where the client's option pill ends, so text
// typed after it is outside the option and "name:" typed inside the value
// stays value text. It is never sent: values exclude it.
inline constexpr char kCommandOptionEnd[] = "\xE2\x81\xA3";
inline constexpr u32 kCommandOptionEndBytes = 3;
// The client's composer option (module 317681 pY): outside every option, a
// known option name not used yet, followed by ':' at the start of the
// arguments, after a blank or right after an option's end. A terminated value
// runs to its kCommandOptionEnd; an unterminated one (text not yet turned into
// chips) to the blank before the next such key, or the end. Byte ranges borrow
// the arguments; value is the trimmed value text. Other text belongs to no option.
struct CommandArgumentSpan {
    JsonValue option;
    Utf8View name, value;
    // key_first..value_first is "name:"; value_first..value_last the raw value;
    // value_last..end_last the end marker (empty while unterminated).
    u32 key_first, value_first, value_last, end_last;
    u32 slot, type;
    bool required, autocomplete;
};
u32 command_argument_spans(const CommandIndex&, const ApplicationCommand&, Utf8View arguments,
                           Slice<CommandArgumentSpan> output) noexcept;
// The client's check of one option value (module 203779): an absent option
// passes unless required; an empty one passes while allow_empty (a command being
// set up). Returns the message to show, empty on success; range messages are
// formatted into message. accepted maps autocomplete choice names to values.
Utf8View command_option_check(const CommandIndex&, JsonValue option, bool present, Utf8View value, bool allow_empty,
                              Slice<char> message, ConstSlice<CommandAcceptedChoice> accepted = {}) noexcept;
// The client's choice ranking (queryChoice): 10 for the whole name, 7 for a
// prefix, 5 for a substring, 3 when every word of the query occurs, 1 for a
// subsequence, else 0 (dropped). ASCII letters compare case-insensitively.
u32 command_choice_score(Utf8View name, Utf8View query) noexcept;
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
// Uses not yet saved, kept on disk as the client persists them
// (ApplicationCommandFrecencyV2 pendingUsages {key, timestamp}) so a restart
// replays them over the next fetched copy. A record: "MCCF", version 1, the
// count, each use (unix ms u64 LE, key length, key), and a 32-bit hash.
inline constexpr u32 kCommandFrecencyPendingUses = 16;
inline constexpr u32 kCommandFrecencyPendingFileBytes =
    10 + kCommandFrecencyPendingUses * (9 + kCommandFrecencyKeyBytes);
struct CommandFrecencyUse {
    u64 at;
    u8 key_bytes;
    char key[kCommandFrecencyKeyBytes];
};
u32 command_frecency_pending_encode(ConstSlice<CommandFrecencyUse> uses, Slice<u8> output) noexcept;
// Rejects a damaged or foreign record, leaving *count untouched.
bool command_frecency_pending_decode(ConstSlice<u8> record, Slice<CommandFrecencyUse> uses, u32* count) noexcept;
} // namespace maicord
