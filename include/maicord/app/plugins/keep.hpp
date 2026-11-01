#pragma once

#include "maicord/core/slice.hpp"

// The host's copy of messages that plugins asked to keep: deleted messages stay visible as
// tombstones and edited ones keep their earlier texts. Session only. A plugin that logs to
// disk does so itself (keep_restore seeds this store from such a log). UI thread only.
namespace maicord {

inline constexpr u32 kKeepMessagesMax = 500;
inline constexpr u32 kKeepPerChannelMax = 32;
inline constexpr u32 kKeepEditsMax = 8;
inline constexpr u32 kKeepContentMax = 4000;
inline constexpr u32 kKeepNameMax = 64;
// Text of every kept message and edit; the oldest messages go first when it is full.
inline constexpr u32 kKeepPoolBytes = 1u << 20;

struct KeepMessage {
    u64 channel_id, message_id, author_id, timestamp_ms, edited_ms;
    Utf8View content, author_name;
};

struct KeepInfo {
    u64 author_id, timestamp_ms, edited_ms;
    u32 edit_count;
    bool deleted;
};

struct KeepStats {
    u32 messages;
    u32 evicted; // messages dropped for a cap since the last clear
};

// Keeps a deleted message as a tombstone. False when it could not be stored (no memory,
// empty content, content over kKeepContentMax).
bool keep_deleted(const KeepMessage&) noexcept;
// Records an edit: `before` becomes the newest earlier text and `after` the latest (the message
// is kept too). kKeepEditsMax earlier texts are kept; the oldest goes first.
bool keep_edited(const KeepMessage& before, Utf8View after, u64 edited_ms) noexcept;
bool keep_is_deleted(u64 message_id) noexcept;
bool keep_info(u64 channel_id, u64 message_id, KeepInfo* info) noexcept;
// version 0: the latest text; 1..edit_count: earlier texts, most recent first. Valid until the next keep call.
bool keep_text(u64 channel_id, u64 message_id, u32 version, Utf8View* text) noexcept;
// Fills output with the channel's tombstones, oldest id first; returns how many there are in all.
// The views are valid until the next keep call.
u32 keep_tombstones(u64 channel_id, KeepMessage* output, u32 capacity) noexcept;
// Appends an older earlier text after the ones already kept (seeding from a plugin's own log, newest first).
bool keep_add_edit(u64 channel_id, u64 message_id, Utf8View text) noexcept;
void keep_forget(u64 message_id) noexcept;
void keep_clear() noexcept;
KeepStats keep_stats() noexcept;

} // namespace maicord
