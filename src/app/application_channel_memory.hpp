#pragma once

#include "maicord/app/channel_memory.hpp"

namespace maicord::app_detail {

// Per-account last channel per server. Loaded when the account starts and
// seeded as the startup restoration target; written debounced, flushed on stop.
void application_channel_memory_start(u64 profile_id) noexcept;
// Writes any pending change and detaches from the account.
void application_channel_memory_stop() noexcept;
// Records selection changes and performs the due write.
void application_channel_memory_pump(u64 now_ms) noexcept;
// Zero entry when the guild has no remembered channel.
ChannelMemoryEntry application_channel_memory_recall(u64 guild_id) noexcept;
// Immediate while a selection change is unobserved, else the write deadline; 0 for none.
u64 application_channel_memory_next_wake() noexcept;

} // namespace maicord::app_detail
