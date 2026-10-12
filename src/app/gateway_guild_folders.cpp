#include "maicord/app/gateway_guild_folders.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/guild_folders.hpp"

namespace maicord {

GatewayClientDispatchRouteResult gateway_client_publish_guild_folders(
    JsonValue settings_proto, bool partial, Slice<char> storage, u64 sequence, bool has_sequence, u64 now_ms,
    bool (*publish)(void*, const GatewayClientEvent&, ConstSlice<u8>) noexcept, void* context) noexcept {
    GatewayClientDispatchRouteResult result{};
    if (!publish || !storage.data || storage.count < kGuildFoldersMaximumGroupBytes) {
        result.error = GatewayClientDispatchRouteError::invalid_input;
        return result;
    }
    JsonString text{};
    if (!json_value_as_string(settings_proto, &text)) {
        result.error = GatewayClientDispatchRouteError::decode;
        return result;
    }
    // The group occupies the front; escaped base64 is unescaped behind it.
    const Slice<u8> group{reinterpret_cast<u8*>(storage.data), kGuildFoldersMaximumGroupBytes};
    Utf8View encoded = text.encoded;
    if (text.has_escapes) {
        const Slice<char> unescaped{storage.data + kGuildFoldersMaximumGroupBytes,
                                    storage.count - kGuildFoldersMaximumGroupBytes};
        const auto decoded = json_string_decode_utf8(text, unescaped);
        if (!decoded.ok()) {
            result.error = GatewayClientDispatchRouteError::payload_capacity;
            return result;
        }
        encoded = {unescaped.data, decoded.bytes};
    }
    u32 bytes = 0;
    bool present = false;
    if (guild_folders_extract_base64(encoded, group, &bytes, &present) != GuildFoldersError::none) {
        result.error = GatewayClientDispatchRouteError::decode;
        return result;
    }
    if (partial && !present)
        return result;
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::guild_folders;
    event.sequence = sequence;
    event.has_sequence = has_sequence;
    event.timestamp_ms = now_ms;
    event.dispatch_payload_bytes = bytes;
    if (!publish(context, event, {group.data, bytes}))
        result.error = GatewayClientDispatchRouteError::payload_capacity;
    return result;
}

} // namespace maicord
