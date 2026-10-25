#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

struct ChatStateStore;

inline constexpr u32 kChannelMemoryCapacity = 64;
// Magic, version, count, then three u64 per entry, then a 32-bit FNV-1a checksum.
inline constexpr u32 kChannelMemoryEntryBytes = 24;
inline constexpr u32 kChannelMemoryFileBytes = 6 + kChannelMemoryCapacity * kChannelMemoryEntryBytes + 4;

// Guild 0 is Home and holds its last conversation. parent_id is a thread's
// parent channel, else 0. IDs only: no names or message content.
struct ChannelMemoryEntry {
    u64 guild_id;
    u64 channel_id;
    u64 parent_id;
};

// Most recently selected first; a full table drops its least recent guild.
struct ChannelMemory {
    ChannelMemoryEntry entries[kChannelMemoryCapacity];
    u8 count;
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
