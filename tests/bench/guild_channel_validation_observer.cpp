#include "guild_channel_validation_observer.hpp"
#include "maicord/model/chat_state.hpp"

namespace maicord::guild_channel_validation_bench {
__declspec(noinline) u64 observe(const ChatStateStore& state,
                                const ChatStateApplyResult& result) noexcept {
    u64 hash = 14695981039346656037ull ^ static_cast<u8>(result.error) ^
        (static_cast<u64>(result.channels) << 32) ^ state.channel_count ^
        state.name_used ^ state.permission_mutations ^ state.selected_channel_id;
    if (state.channel_count) {
        const auto& first = state.channels[state.channel_order[0]];
        const auto& last = state.channels[state.channel_order[state.channel_count - 1]];
        hash ^= first.id ^ last.id ^ first.effective_permissions ^ last.last_message_id;
    }
    return hash * 1099511628211ull;
}
}
