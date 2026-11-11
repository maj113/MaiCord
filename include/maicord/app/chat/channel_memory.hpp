#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

struct ChatStateStore;

inline constexpr u32 kChannelMemoryCapacity = 64;
// Titlebar tabs persist beside the table.
inline constexpr u32 kChannelMemoryTabCapacity = 16;
inline constexpr u32 kChannelMemoryTabLabelBytes = 40;
// Magic, version, count, then three u64 per entry, then (version 2, written only with tabs) the tab count and per
// tab its channel and guild ids, the length of its label and the label, then a 32-bit FNV-1a checksum.
inline constexpr u32 kChannelMemoryEntryBytes = 24;
inline constexpr u32 kChannelMemoryTabBytes = 17 + kChannelMemoryTabLabelBytes;
inline constexpr u32 kChannelMemoryFileBytes =
    6 + kChannelMemoryCapacity * kChannelMemoryEntryBytes + 1 + kChannelMemoryTabCapacity * kChannelMemoryTabBytes + 4;

// A tab: its channel and that channel's guild (0 for a DM), and the name it last showed, which a tab whose
// channel has not loaded yet (an unavailable guild) keeps showing.
struct ChannelMemoryTab {
    u64 channel_id;
    u64 guild_id;
    u8 label_bytes;
    char label[kChannelMemoryTabLabelBytes];
};

// Guild 0 is Home and holds its last conversation. parent_id is a thread's
// parent channel, else 0. IDs only: no names or message content.
struct ChannelMemoryEntry {
    u64 guild_id;
    u64 channel_id;
    u64 parent_id;
};

// Most recently selected first; a full table drops its least recent guild. `tabs` are the open channel tabs in
// strip order, with no duplicate and no zero.
struct ChannelMemory {
    ChannelMemoryEntry entries[kChannelMemoryCapacity];
    ChannelMemoryTab tabs[kChannelMemoryTabCapacity];
    u8 count;
    u8 tab_count;
    u32 revision;
};

// Moves the guild to the front. False (and no revision change) when it already
// leads with the same channel; false as well for a zero channel or a parent
// equal to the channel.
bool channel_memory_remember(ChannelMemory*, u64 guild_id, u64 channel_id, u64 parent_id) noexcept;
const ChannelMemoryEntry* channel_memory_find(const ChannelMemory&, u64 guild_id) noexcept;
// The last selection of the previous run, or null for an empty table.
const ChannelMemoryEntry* channel_memory_latest(const ChannelMemory&) noexcept;
// Records the store's selected channel. Nothing is recorded while no channel
// is selected or a restoration is pending. True when the table changed.
bool channel_memory_observe(ChannelMemory*, const ChatStateStore&) noexcept;
// Replaces the tab list. False (and no revision change) when it equals the stored one, or when it holds more than
// the capacity, a zero channel or a duplicate.
bool channel_memory_set_tabs(ChannelMemory*, ConstSlice<ChannelMemoryTab> tabs) noexcept;
// Startup: makes the latest selection the store's restoration target, so READY
// and its supplemental state place it, falling back to the guild's default
// channel when it is gone or unreadable, and primes Home's last conversation.
void channel_memory_seed_restore(const ChannelMemory&, ChatStateStore*) noexcept;
u32 channel_memory_encode(const ChannelMemory&, Slice<u8> output) noexcept;
// Strict: a malformed, short, oversized, duplicated or checksum-failing record is rejected whole
// and leaves the table untouched.
bool channel_memory_decode(ConstSlice<u8> input, ChannelMemory*) noexcept;

// %LOCALAPPDATA%\MaiCord\accounts\channels-<profile>.last; the directory is never created here.
bool channel_memory_path(u64 profile_id, wchar_t* path, u32 capacity) noexcept;
// A missing file loads an empty table; a corrupt file loads an empty table and returns false.
bool channel_memory_load_file(const wchar_t* path, ChannelMemory*) noexcept;
bool channel_memory_save_file(const wchar_t* path, const ChannelMemory&) noexcept;

} // namespace maicord
