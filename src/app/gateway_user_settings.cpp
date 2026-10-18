#include "maicord/app/gateway_user_settings.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/user_settings_proto.hpp"

namespace maicord {

GatewayClientDispatchRouteResult gateway_client_publish_user_settings(
    JsonValue settings_proto, JsonValue session_hash, bool partial, Slice<char> storage, u64 sequence,
    bool has_sequence, u64 now_ms, bool (*publish)(void*, const GatewayClientEvent&, ConstSlice<u8>) noexcept,
    void* context) noexcept {
    GatewayClientDispatchRouteResult result{};
    constexpr u32 output_bytes = kUserSettingsProtoArenaBytes + 16;
    if (!publish || !storage.data || storage.count < output_bytes) {
        result.error = GatewayClientDispatchRouteError::invalid_input;
        return result;
    }
    // A READY without settings (missing or "") is an empty message: every
    // setting at its default and editable, as the official client loads it.
    JsonString text{};
    if ((partial || settings_proto.source.data) && !json_value_as_string(settings_proto, &text)) {
        result.error = GatewayClientDispatchRouteError::decode;
        return result;
    }
    // Groups occupy the front; escaped base64 is unescaped behind them.
    Utf8View encoded = text.encoded;
    if (text.has_escapes) {
        const auto decoded = json_string_decode_utf8(text, {storage.data + output_bytes, storage.count - output_bytes});
        if (!decoded.ok()) {
            result.error = GatewayClientDispatchRouteError::payload_capacity;
            return result;
        }
        encoded = {storage.data + output_bytes, decoded.bytes};
    }
    char hash_storage[kUserSettingsSessionHashBytes];
    Utf8View hash{};
    JsonString hash_text{};
    if (session_hash.source.data && json_value_as_string(session_hash, &hash_text)) {
        const auto decoded = json_string_decode_utf8(hash_text, {hash_storage, sizeof(hash_storage)});
        if (decoded.ok())
            hash = {hash_storage, decoded.bytes};
    }
    if (!encoded.bytes && partial)
        return result;
    const Slice<u8> output{reinterpret_cast<u8*>(storage.data), output_bytes};
    const auto extracted = user_settings_proto_extract_base64(encoded, kUserSettingsStoredGroups, hash,
                                                              {output.data, kUserSettingsProtoArenaBytes});
    if (!extracted.ok()) {
        result.error = GatewayClientDispatchRouteError::decode;
        return result;
    }
    if (partial && !extracted.present_mask)
        return result;
    u32 bytes = extracted.bytes;
    if (partial) {
        // Tag kUserSettingsPartialField (varint), value 1.
        constexpr u32 key = kUserSettingsPartialField << 3;
        output.data[bytes++] = static_cast<u8>((key & 127) | 128);
        output.data[bytes++] = static_cast<u8>(key >> 7);
        output.data[bytes++] = 1;
    }
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::user_settings;
    event.sequence = sequence;
    event.has_sequence = has_sequence;
    event.timestamp_ms = now_ms;
    event.dispatch_payload_bytes = bytes;
    if (!publish(context, event, {output.data, bytes}))
        result.error = GatewayClientDispatchRouteError::payload_capacity;
    return result;
}

} // namespace maicord
