#pragma once
#include "maicord/app/gateway_client.hpp"

namespace maicord {

// Publishes one user_settings dispatch whose payload holds the stored
// PreloadedUserSettings groups (user_settings_proto.hpp), the session hash
// when given, and the partial marker for USER_SETTINGS_PROTO_UPDATE. An
// empty or groupless partial update publishes nothing. storage must not
// alias the JSON sources.
GatewayClientDispatchRouteResult gateway_client_publish_user_settings(
    JsonValue settings_proto, JsonValue session_hash, bool partial, Slice<char> storage, u64 sequence,
    bool has_sequence, u64 now_ms, bool (*publish)(void*, const GatewayClientEvent&, ConstSlice<u8>) noexcept,
    void* context) noexcept;

} // namespace maicord
