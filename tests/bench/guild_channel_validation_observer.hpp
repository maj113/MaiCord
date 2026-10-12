#pragma once
#include "maicord/app/chat_state_dispatch.hpp"

namespace maicord::guild_channel_validation_bench {
// Separate translation unit, /GL-, no unity build. Exact rows, names and
// complete rejection state are checked outside the measured intervals.
u64 observe(const ChatStateStore&, const ChatStateApplyResult&) noexcept;
}
