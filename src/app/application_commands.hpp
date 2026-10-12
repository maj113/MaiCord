#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/ui/scroll_host.hpp"
namespace maicord::app_detail {
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
} // namespace maicord::app_detail
