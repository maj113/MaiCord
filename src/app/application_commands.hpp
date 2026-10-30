#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/ui/scroll_host.hpp"
namespace maicord { struct MessageComposerProjectionContext; }
namespace maicord::app_detail {
void application_commands_composer(MessageComposerProjectionContext*) noexcept;
bool application_commands_sync(bool* handled) noexcept;
bool application_commands_activate(u32 action) noexcept;
bool application_commands_send(bool* handled) noexcept;
bool application_commands_submit(u64 now) noexcept;
bool application_commands_pending(u64 now) noexcept;
bool application_commands_response(const RestClientResponse&, Utf8View body) noexcept;
bool application_commands_scroll_range(bool rail, UiScrollRange*) noexcept;
bool application_commands_scroll(bool rail, double offset, bool* changed) noexcept;
bool application_commands_key(u32 key) noexcept;
void application_commands_gateway(Utf8View body) noexcept;
// Gateway INTERACTION_CREATE / SUCCESS / FAILURE / MODAL_CREATE (verbatim
// object) and a created message's nonce advance the local command message
// ("Sending command...", "<app> is thinking...", failure text) of a command
// sent from this session, or remove it once answered.
void application_commands_interaction(GatewayClientDispatchKind kind, Utf8View body) noexcept;
void application_commands_message_created(u64 nonce) noexcept;
// "Retry send" on a failed local command message: sends the same command data
// again with a new nonce and local message. False when the message is not a
// command echo (an ordinary send retry applies).
bool application_commands_retry(u64 message_id) noexcept;
// Invalidates the matching guild index, or a bound DM index conservatively
// (no guild-to-bot map is retained). The user index is independent. UI refresh
// is deferred until tick; queued/sending interactions and the draft stay intact.
void application_commands_invalidate_guild(u64 guild_id) noexcept;
bool application_commands_tick(u64 now) noexcept;
void application_commands_dismiss() noexcept;
u64 application_commands_next_wake() noexcept;
void application_commands_release() noexcept;
// User (type 2) and message (type 3) context menu commands available in the
// active channel. Loads the channel's command index when needed; *loading
// reports an index still on its way (application_user_actions_commands_changed
// fires when it lands).
struct ApplicationContextCommand {
    Utf8View name, application_name;
    u64 command_id, application_id;
};
u32 application_commands_context(u8 type, Slice<ApplicationContextCommand> output, bool* loading) noexcept;
// Runs one context menu command on a user or message through the command lane.
bool application_commands_context_invoke(u8 type, u64 command_id, u64 target_id) noexcept;
// One app's root chat commands from the loaded user and channel indexes (the
// user index wins a shared command), in index order, deduplicated by id, for
// the active channel only and only with Use Application Commands. Starts
// loading missing indexes without refetching loaded ones; *loading reports an
// index on its way. Views borrow the index until the catalog revision changes;
// a command with subcommands has no description.
struct ApplicationRootCommand {
    Utf8View name, description;
    u64 command_id;
    bool nsfw, subcommands;
};
u32 application_commands_for_application(u64 channel_id, u64 application_id, Slice<ApplicationRootCommand> output,
                                         bool* loading) noexcept;
// Advances whenever an index lands or is invalidated.
u32 application_commands_catalog_revision() noexcept;
// As a click on a profile's command or a </name:id> mention: a non-empty draft
// first asks "Clear message?"; then the active draft becomes "/<name> " and the
// command with that id and full name (of any app when application_id is 0) is
// selected once its index is loaded. False for invalid input or an internal failure.
bool application_commands_insert(u64 application_id, u64 command_id, Utf8View name) noexcept;
} // namespace maicord::app_detail
