#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/application_commands.hpp"

namespace maicord::app_detail {
// The official client's own "Built-in" commands, its last picker section.
// They never reach an application: text commands rewrite the message that is
// then sent normally, /gif and /sticker open their searches, /leave asks to
// leave the group, /msg opens the DM and sends there, and /nick, /thread,
// /kick, /ban and /timeout call the API on the command lane, answering with a
// local notice as the client's bot message does.
inline constexpr u64 kBuiltinCommandApplication = 1;
// The section as a command index (application "Built-in"), decoded once.
const CommandIndex* builtin_commands_index() noexcept;
// The client's availability rule of each command in this channel.
bool builtin_command_available(const ApplicationCommand&, u64 channel_id) noexcept;
enum class BuiltinCommandRun : u8 {
    failed,
    // *text is the message to send in place of the draft.
    text,
    handled,
};
// options_json: command_arguments' options array. Text storage holds *text.
BuiltinCommandRun builtin_command_run(const ApplicationCommand&, Utf8View options_json, u64 channel_id,
                                      Slice<char> text_storage, Utf8View* text) noexcept;
// The command lane carries the network built-ins.
bool builtin_commands_pending() noexcept;
// Submits the queued action with the lane's request id; false when nothing was sent.
bool builtin_commands_submit(u64 request_id, bool* busy) noexcept;
bool builtin_commands_response(const RestClientResponse&, Utf8View body) noexcept;
// Sends a waiting /msg or /thread message once its channel is there.
bool builtin_commands_tick() noexcept;
void builtin_commands_release() noexcept;
} // namespace maicord::app_detail
