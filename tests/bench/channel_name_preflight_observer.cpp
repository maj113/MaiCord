#include "channel_name_preflight_observer.hpp"

namespace maicord::channel_name_preflight_bench {
__declspec(noinline) u64 observe_preflight(const ChatStateStore& state,
    const ChannelInput& input, u32 ordinal) noexcept {
    const auto error = chat_state_store_preflight_channel(state, input);
    return (kObservationSeed ^ static_cast<u64>(error) ^ ordinal ^ input.id ^
        (static_cast<u64>(state.channel_count) << 32) ^ state.role_count ^
        input.name.bytes ^ state.name_used) * 1099511628211ull;
}
}
