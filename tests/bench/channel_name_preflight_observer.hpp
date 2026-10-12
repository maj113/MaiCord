#pragma once
#include "maicord/model/chat_state.hpp"

namespace maicord::channel_name_preflight_bench {
inline constexpr u64 kObservationSeed = 14695981039346656037ull;
// Compile separately with /GL-; do not unity-build. The opaque boundary calls
// the actual production API on every iteration, preventing LTCG hoisting.
__declspec(noinline) u64 observe_preflight(const ChatStateStore&,
    const ChannelInput&, u32 ordinal) noexcept;
}
