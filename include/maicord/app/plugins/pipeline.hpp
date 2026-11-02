#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/sdk/maicord_plugin.h"

// The message pipeline for plugins (design section 8, T1): the hooks around messages (click,
// deleted, edited, typing), the batched event feed, timers, and the actions plugins may take
// (sends, edits, reactions, local messages, composer control). The runtime half (pipeline.cpp,
// pipeline_api.cpp) is independent of the application: it runs hooks and enforces permissions,
// gestures and rate limits, and reaches the application through PluginPipelineSink.
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

// ---- Events -----------------------------------------------------------------------

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

// ---- Timers -----------------------------------------------------------------------

// The earliest due time on the monotonic millisecond clock, or 0 with no timers.
u64 plugin_timers_next_due() noexcept;
// Calls TIMER hooks of due timers (at most kPluginEventCallsPerPass per plugin); returns how many calls ran.
u32 plugin_timers_poll(u64 now_ms) noexcept;

// ---- Application side of the actions ----------------------------------------------------

// What the application does for a plugin; every function runs after the runtime checked
// permission, gesture, arguments and rate limit. A null function makes the call fail with
// MC_E_UNSUPPORTED. Status values are MC_OK or MC_E_*.
struct PluginSend {
    u64 channel_id, reply_to;
    Utf8View text;
    u32 flags; // MC_DRAFT_SILENT, MC_DRAFT_SUPPRESS_EMBEDS, MC_DRAFT_REPLY_PING
};
struct PluginPipelineSink {
    mc_status (*send)(u64 owner_id, const PluginSend&) noexcept;
    mc_status (*edit)(u64 owner_id, u64 channel_id, u64 message_id, Utf8View text) noexcept;
    mc_status (*react)(u64 owner_id, u64 channel_id, u64 message_id, Utf8View emoji, bool remove) noexcept;
    mc_status (*delete_own)(u64 owner_id, u64 channel_id, u64 message_id) noexcept;
    mc_status (*mark_read)(u64 owner_id, u32 scope, u64 id) noexcept;
    // activity is null to clear.
    mc_status (*set_activity)(u64 owner_id, const mc_activity* activity) noexcept;
    mc_status (*local_message)(u64 owner_id, u64 channel_id, Utf8View text, u64* message_id) noexcept;
    mc_status (*local_message_remove)(u64 owner_id, u64 channel_id, u64 message_id) noexcept;
    mc_status (*composer_text)(void* output, u32 capacity, u32* bytes) noexcept;
    mc_status (*composer_insert)(Utf8View text) noexcept;
    mc_status (*composer_replace)(Utf8View text) noexcept;
    mc_status (*composer_focus)() noexcept;
    mc_status (*composer_reply)(u64 channel_id, u64 message_id, bool ping) noexcept;
    mc_status (*composer_edit)(u64 channel_id, u64 message_id) noexcept;
    // The kept messages changed outside a hook (keep_restore): repaint.
    void (*keep_changed)() noexcept;
};
// The sink is global (one application); null removes it. Tests install their own.
void plugin_pipeline_set_sink(const PluginPipelineSink*) noexcept;

// Counters since process start; for tests and the dev console.
struct PluginPipelineStats {
    u32 deleted_calls, edited_calls, click_calls, typing_calls;
    u32 event_calls;       // EVENTS hook calls (batches)
    u32 events_delivered;  // events handed to plugins, overflow notices excluded
    u32 events_lost;       // events a slow plugin missed
    u32 timer_calls;
    u32 strikes;           // calls past their budget
    u32 actions;           // actions that reached the sink
    u32 action_denials;    // actions refused for permission, gesture or rate limit
};
const PluginPipelineStats& plugin_pipeline_stats() noexcept;

// Message actions (send, edit, delete) are limited per plugin to this many in one window; reactions, mark
// read, activity and local messages have their own limits (pipeline.cpp).
inline constexpr u32 kPluginActionWindowMilliseconds = 5000;
inline constexpr u32 kPluginActionsPerWindow = 5;

} // namespace maicord
