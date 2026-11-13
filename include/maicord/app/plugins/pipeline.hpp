#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/sdk/maicord_plugin.h"

// The message pipeline for plugins: the hooks around messages (click,
// deleted, edited, typing), the batched event feed, timers, and the actions plugins may take
// (sends, edits, reactions, local messages, composer control). The runtime half (pipeline.cpp,
// pipeline_api.cpp) is independent of the application: it runs hooks and enforces permissions,
// gestures and rate limits, and reaches the application through the plugin_host_* functions below.
// Kept (deleted and edited) messages live in keep.hpp. UI thread only.
namespace maicord {

// A message as hooks see it; flags are MC_MESSAGE_*.
struct PluginMessage {
    u64 message_id, channel_id, guild_id, author_id, reply_to, timestamp_ms, edited_ms;
    Utf8View content, author_name;
    u32 flags, type, revision, relationship, attachment_count, embed_count;
};

struct PluginClick {
    u32 button, count, modifiers; // MC_CLICK_LEFT/MIDDLE, 1 or 2, MC_MOD_*
};

// MESSAGE_DELETED, called before the application forgets the message. When any plugin keeps it
// the message is stored (keep.hpp) and true is returned; the caller then leaves it on screen.
// Every subscriber is called.
bool plugin_dispatch_deleted(const PluginMessage&) noexcept;
// MESSAGE_EDITED: when any plugin keeps it, `before.content` joins the message's earlier texts and
// `after.content` becomes the latest.
bool plugin_dispatch_edited(const PluginMessage& before, const PluginMessage& after) noexcept;
// MESSAGE_CLICK: true when a plugin handled the click and the application must skip its own.
// Subscribers whose filter excludes this click are not called.
bool plugin_dispatch_click(const PluginMessage&, const PluginClick&) noexcept;
// PRE_TYPING: true when a plugin suppresses the typing notice (the first one that does ends the pass).
bool plugin_dispatch_typing(u64 channel_id) noexcept;

// One gateway change as the EVENTS hook sees it. Text and name are copied by push.
struct PluginEvent {
    u32 kind, flags; // MC_EVENT_*, MC_EVENT_* flags
    u64 guild_id, channel_id, message_id, user_id, value, time_ms;
    Utf8View text, name;
};
inline constexpr u32 kPluginEventSlots = 128;
inline constexpr u32 kPluginEventTextBytes = 512;
inline constexpr u32 kPluginEventNameBytes = 64;
inline constexpr u32 kPluginEventCallsPerPass = 4; // per plugin; the rest waits for the next pass

// The union of the enabled plugins' EVENTS filters: MC_EVENT_BIT(kind) for every kind some
// plugin wants. Translating an event nobody wants is wasted work.
u64 plugin_events_interest() noexcept;
// Queues an event for every subscriber whose filter wants its kind. Text over the slot is cut and
// marked MC_EVENT_TRUNCATED. A plugin that falls kPluginEventSlots behind is told how many it lost.
bool plugin_events_push(const PluginEvent&) noexcept;
// Delivers queued events in batches of MC_EVENT_BATCH_MAX; true while some plugin still has more.
bool plugin_events_deliver() noexcept;
bool plugin_events_pending() noexcept;

// The earliest due time on the monotonic millisecond clock, or 0 with no timers.
u64 plugin_timers_next_due() noexcept;
// Calls TIMER hooks of due timers (at most kPluginEventCallsPerPass per plugin); returns how many calls ran.
u32 plugin_timers_poll(u64 now_ms) noexcept;

struct PluginSend {
    u64 channel_id, reply_to;
    Utf8View text;
    u32 flags; // MC_DRAFT_SILENT, MC_DRAFT_SUPPRESS_EMBEDS, MC_DRAFT_REPLY_PING
};
// What the application does for a plugin (pipeline_host.cpp); the runtime calls these by name, every one after it
// checked permission, gesture, arguments and rate limit. A test that links runtime objects links a .cpp of its own
// that defines all sixteen, in place of pipeline_host.cpp. Status values are MC_OK or MC_E_*.
mc_status plugin_host_send(u64 owner_id, const PluginSend&) noexcept;
mc_status plugin_host_edit(u64 owner_id, u64 channel_id, u64 message_id, Utf8View text) noexcept;
mc_status plugin_host_react(u64 owner_id, u64 channel_id, u64 message_id, Utf8View emoji, bool remove) noexcept;
mc_status plugin_host_delete_own(u64 owner_id, u64 channel_id, u64 message_id) noexcept;
mc_status plugin_host_mark_read(u64 owner_id, u32 scope, u64 id) noexcept;
// activity is null to clear.
mc_status plugin_host_set_activity(u64 owner_id, const mc_activity* activity) noexcept;
mc_status plugin_host_local_message(u64 owner_id, u64 channel_id, Utf8View text, u64* message_id) noexcept;
mc_status plugin_host_local_message_remove(u64 owner_id, u64 channel_id, u64 message_id) noexcept;
mc_status plugin_host_composer_text(void* output, u32 capacity, u32* bytes) noexcept;
mc_status plugin_host_composer_insert(Utf8View text) noexcept;
mc_status plugin_host_composer_replace(Utf8View text) noexcept;
mc_status plugin_host_composer_focus() noexcept;
mc_status plugin_host_composer_reply(u64 channel_id, u64 message_id, bool ping) noexcept;
mc_status plugin_host_composer_edit(u64 channel_id, u64 message_id) noexcept;
// The kept messages changed outside a hook (keep_restore): repaint.
void plugin_host_keep_changed() noexcept;
// read_open_channel: fills out (zeroed); MC_E_NOT_FOUND without an open channel.
mc_status plugin_host_read_open_channel(mc_open_channel* out) noexcept;
// read_messages: up to capacity loaded messages of channel below before (0: the newest), newest first.
mc_status plugin_host_read_messages(u64 channel_id, u64 before, mc_message_ref* out, u32 capacity, u32* count) noexcept;

// Message actions (send, edit, delete) are limited per plugin to this many in one window; reactions, mark
// read, activity and local messages have their own limits (pipeline.cpp).
inline constexpr u32 kPluginActionWindowMilliseconds = 5000;
inline constexpr u32 kPluginActionsPerWindow = 5;

} // namespace maicord
