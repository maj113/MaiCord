#pragma once
#include "maicord/app/gateway_client.hpp"

namespace maicord {

// Publishes one guild_folders dispatch whose payload is the verbatim GuildFolders
// group from a base64 PreloadedUserSettings JSON string (empty when absent).
// A partial update without the field publishes nothing. storage must not alias
// the JSON source and holds the group plus any unescaped base64 text.
GatewayClientDispatchRouteResult gateway_client_publish_guild_folders(
    JsonValue settings_proto, bool partial, Slice<char> storage, u64 sequence, bool has_sequence, u64 now_ms,
    bool (*publish)(void*, const GatewayClientEvent&, ConstSlice<u8>) noexcept, void* context) noexcept;

} // namespace maicord
