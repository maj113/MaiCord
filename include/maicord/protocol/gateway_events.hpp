#pragma once

#include "maicord/protocol/gateway_payload.hpp"
#include "maicord/protocol/account_requirement.hpp"

namespace maicord {

struct GatewayHello {
    u32 heartbeat_interval_ms;
};

struct GatewayRateLimited {
    u64 guild_id;
    u64 retry_after_ms;
    // Zero when the optional nonce is absent or is not our decimal nonce format.
    u64 nonce;
    u32 opcode;
};

struct GatewayMemberChunkReceipt {
    u64 guild_id;
    u64 nonce;
    u32 chunk_index;
    u32 chunk_count;
};

struct GatewayReady {
    u64 user_id;
    u32 api_version;
    JsonString session_id;
    JsonString resume_gateway_url;
    JsonValue user;
    JsonValue guilds;
    JsonValue private_channels;
    JsonValue users;
    JsonValue apex_experiments;
    JsonValue regional_feature_config;
    JsonValue linked_users;
    AccountRequirement required_action;
    JsonValue relationships;
    JsonValue merged_presences;
    JsonValue merged_members;
    JsonValue read_state;
    JsonValue user_guild_settings, user_settings_proto, notification_settings;
    JsonValue auth_session_id_hash;
    bool notification_policy_invalid;
};

struct GatewayResumed {
    JsonValue trace;
    bool has_trace;
};

enum class GatewayEventDecodeError : u8 {
    none,
    invalid_input,
    invalid_data,
    scan_failed,
    invalid_json,
    duplicate_field,
    missing_heartbeat_interval,
    invalid_heartbeat_interval,
    missing_required_field,
    invalid_api_version,
    invalid_session_id,
    invalid_resume_gateway_url,
    invalid_user,
    invalid_guilds,
    invalid_trace,
    invalid_resumable,
    invalid_message_id,
    invalid_channel_id,
    invalid_author,
    invalid_author_id,
    invalid_message_content,
    message_content_too_large,
    message_content_capacity,
    invalid_message_timestamp,
    invalid_edited_timestamp,
    invalid_message_flags,
    invalid_message_ids,
    message_id_capacity,
    mention_role_capacity,
    mention_user_capacity,
    invalid_user_name,
    invalid_message_nonce,
};

struct GatewayEventDecodeResult {
    u32 error_offset;
    GatewayEventDecodeError error;
    JsonScanError scan_error;
    JsonReadError read_error;
    char schema_field[32];

    constexpr bool ok() const noexcept {
        return error == GatewayEventDecodeError::none;
    }
};

GatewayEventDecodeResult gateway_decode_hello(JsonValue data, Slice<u32> structural_scratch,
                                              u32 maximum_depth, GatewayHello* output) noexcept;
// Bounded, allocation-free decoder. Decimal seconds round up to milliseconds.
// Unknown fields are ignored; malformed/duplicate fields leave output unchanged.
GatewayEventDecodeResult gateway_decode_rate_limited(JsonValue data, Slice<u32> structural_scratch,
                                                     u32 maximum_depth,
                                                     GatewayRateLimited* output) noexcept;
// Only completion metadata is copied; member rows retain their existing decoder.
GatewayEventDecodeResult gateway_decode_member_chunk_receipt(JsonValue data, Slice<u32> structural_scratch,
                                                             u32 maximum_depth,
                                                             GatewayMemberChunkReceipt* output) noexcept;
GatewayEventDecodeResult gateway_decode_ready(JsonValue data, Slice<u32> structural_scratch,
                                              u32 maximum_depth, GatewayReady* output) noexcept;
GatewayEventDecodeResult gateway_decode_supplemental_private_channels(JsonValue data, Slice<u32> scratch,
    u32 maximum_depth, JsonValue* channels) noexcept;
GatewayEventDecodeResult gateway_decode_resumed(JsonValue data, Slice<u32> structural_scratch,
                                                u32 maximum_depth, GatewayResumed* output) noexcept;
GatewayEventDecodeResult gateway_decode_invalid_session(JsonValue data, bool* resumable) noexcept;

} // namespace maicord
