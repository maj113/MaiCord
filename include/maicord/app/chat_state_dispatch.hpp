#pragma once

#include "maicord/app/gateway_client.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/guild_detail_store.hpp"
#include "maicord/model/user_store.hpp"

namespace maicord {

enum class ChatStateApplyError : u8 {
    none,
    invalid_input,
    unsupported_event,
    invalid_payload,
    capacity,
};

struct ChatStateApplyResult {
    ChatStateStoreError store_error;
    ChatStateApplyError error;
    u32 guilds;
    u32 channels;
    u32 roles;
    u32 overwrites;
    u32 emojis;
    bool selection_changed;

    constexpr bool ok() const noexcept {
        return error == ChatStateApplyError::none;
    }
};

bool gateway_client_dispatch_is_state(GatewayClientDispatchKind kind) noexcept;
// Typed current-self authority is independent of the selected participant list.
// The complete payload is preflighted before any cached channel proof changes.
ChatStateApplyResult chat_state_apply_thread_authority(
    const GatewayClientEvent&, ConstSlice<u8>, ChatStateStore*) noexcept;
// Accepts a complete routed event, including validated notification metadata.
// Ordinary state uses its exact body prefix; malformed metadata publishes no
// state mutation. Already normalized ordinary events remain accepted.
ChatStateApplyResult chat_state_apply_gateway_event(ChatStateStore* store,
                                                          const GatewayClientEvent& event,
                                                          ConstSlice<u8> payload,
                                                          Slice<u64> guild_id_scratch,
                                                          UserStore* users = nullptr) noexcept;
// Server details of a guild create/update, applied after `state` took the
// same event so its permissions decide whether the raw feature names are
// kept: with Manage Server, or while permissions are still unresolved. A
// guild delete that is not an outage drops the guild and READY clears the
// store. True when the store changed.
bool guild_detail_store_apply_gateway_event(GuildDetailStore* store, const ChatStateStore& state,
                                            const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept;

} // namespace maicord
