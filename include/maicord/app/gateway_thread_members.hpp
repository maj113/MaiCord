#pragma once
#include "maicord/app/gateway_client.hpp"
#include "maicord/model/thread_member_store.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/user_store.hpp"
namespace maicord {
// False is malformed input or failed snapshot backing; unowned scopes are a
// validated no-op. Failed backing preserves the prior identity snapshot. The entire
// packed packet is preflighted before either profile or identity application.
// Complete routed events may carry notification metadata; admission validates
// it once and uses the unchanged ordinary participant body prefix.
bool gateway_client_apply_thread_members(const GatewayClientEvent& event,
    ConstSlice<u8> payload, ThreadMemberStore* threads,
    GuildMemberStore* members, UserStore* users,
    const ChatStateStore& state) noexcept;
}
