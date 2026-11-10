#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/platform/window/event.hpp"

// The application's side of the plugin message pipeline (include/maicord/app/plugins/pipeline.hpp):
// it feeds gateway changes, clicks and typing to the hooks, answers the actions plugins take (the plugin_host_*
// functions of pipeline.hpp, defined in pipeline_host.cpp), and keeps the kept messages (keep.hpp) on screen.
namespace maicord {
struct GatewayClientEvent;
namespace app_detail {

// Idempotent: turns on the live timeline's tombstone query, once a plugin keeps or listens.
void application_plugins_pipeline_install() noexcept;
// One popped Gateway event, before the stores apply it: MESSAGE_DELETED and MESSAGE_EDITED hooks see the
// cached copy, and the event feed queues what some plugin asked for. A no-op without a subscriber.
void application_plugins_pipeline_gateway(const GatewayClientEvent&, ConstSlice<u8> payload) noexcept;
// Once per loop pass: what plugins changed since the UI last built from it (decorations, buttons), delivers
// queued events, runs due timers, puts tombstones back on screen and clears an expired composer notice.
void application_plugins_poll() noexcept;
// Milliseconds until plugins need a pass (0: now, INFINITE: never).
u32 application_plugins_wait_timeout(u64 now_ms) noexcept;
// A click on the message under the pointer (a left press, a left double click or a middle press).
// True when a plugin handled it and the application must skip its own handling.
bool application_plugins_message_click(const WindowEvent& event, bool middle, u32 count) noexcept;
// True when a plugin suppresses the typing notice for the channel.
bool application_plugins_typing_suppressed(u64 channel_id) noexcept;
// A plugin asked the user to confirm the send of the composer's text; the send continues when accepted.
bool application_plugins_send_confirm(Utf8View question) noexcept;
// A pre_send hook asked for the next send to suppress link embeds; the send takes it (once).
void application_plugins_suppress_embeds_next(bool suppress) noexcept;
bool application_plugins_take_suppress_embeds() noexcept;
// Kept messages and plugin state belong to the account: drop them when it goes.
void application_plugins_pipeline_account_invalidated() noexcept;
// A removal this MaiCord asked for (leave or delete a server, leave a group DM, end a relationship): the
// event that confirms it within a minute carries MC_EVENT_OWN. A no-op unless a plugin listens for the kind.
enum class PluginOwnRemoval : u8 { channel = 10, guild = 13, relationship = 18 }; // MC_EVENT_* kinds
void application_plugins_own_removal(PluginOwnRemoval kind, u64 id) noexcept;

} // namespace app_detail
} // namespace maicord
