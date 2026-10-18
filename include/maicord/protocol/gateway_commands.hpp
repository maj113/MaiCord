#pragma once

#include "maicord/core/utf.hpp"

namespace maicord {

enum class GatewayCommandError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    value_too_large,
    output_too_small,
};

struct GatewayCommandWriteResult {
    u32 bytes;
    GatewayCommandError error;

    constexpr bool ok() const noexcept {
        return error == GatewayCommandError::none;
    }
};

struct GatewayResume {
    Utf8View token;
    Utf8View session_id;
    u64 sequence;
};

inline constexpr u32 kGatewayMemberSubscriptionMaximumRanges = 5;
struct GatewayMemberSubscriptionRange {
    u32 first;
    u32 last;
};

// The five most recent channels of a guild stay subscribed and are all written
// in every frame, most recent first.
inline constexpr u32 kGatewayGuildSubscriptionMaximumChannels = 5;
struct GatewayChannelSubscription {
    u64 channel_id;
    GatewayMemberSubscriptionRange ranges[kGatewayMemberSubscriptionMaximumRanges];
    u32 range_count;
};

struct GatewayGuildSubscription {
    u64 guild_id;
    u64 channel_id;
    u32 range_first;
    u32 range_last;
    bool typing;
    bool activities;
    bool threads;
    GatewayMemberSubscriptionRange ranges[kGatewayMemberSubscriptionMaximumRanges];
    u32 range_count;
    // A present field replaces the thread interest stored for the guild; zero writes an empty [].
    u64 thread_id;
    // Internal worker cleanup only. Public subscription requests reject it.
    u64 clear_thread_guild_id;
    bool thread_member_lists_present;
    // Thread-only frames omit channels and require channel_id/range_count zero.
    bool omit_channel_ranges;
    // Releasing interest keeps the guild's existing feature flags.
    bool omit_features;
    // The guild's other remembered channels after channel_id, most recent
    // first; each keeps at least one range.
    GatewayChannelSubscription channels[kGatewayGuildSubscriptionMaximumChannels - 1];
    u32 channel_count;
};

// Auth-token refresh (bit 8) stays disabled until a replacement can be adopted
// atomically by REST, Gateway and encrypted profile storage.
inline constexpr u32 kGatewayUserCapabilitiesDefault = 1'734'397;
inline constexpr u32 kGatewayUserMaximumCommandBytes = 4'096;

inline constexpr u32 kGatewayGuildMemberRequestMaximum = 100;
inline constexpr u32 kGatewayGuildMemberQueryMaximumBytes = 100;
// channel_id scopes queued UI interest; it is not sent in opcode 8.
struct GatewayGuildMemberRequest {
    u64 guild_id;
    u64 channel_id;
    u64 nonce;
    u64 user_ids[kGatewayGuildMemberRequestMaximum];
    u32 user_count;
    // A name search instead (user_count zero, limit 1-100): opcode 8 carries
    // "query" (UTF-8, possibly empty) and "limit" in place of user_ids.
    u32 limit;
    u32 query_bytes;
    char query[kGatewayGuildMemberQueryMaximumBytes];
};
constexpr bool gateway_guild_member_request_is_query(const GatewayGuildMemberRequest& request) noexcept {
    return request.user_count == 0 && request.limit != 0;
}
bool gateway_guild_member_request_valid(const GatewayGuildMemberRequest&) noexcept;
GatewayCommandWriteResult gateway_write_guild_member_request(const GatewayGuildMemberRequest&,
                                                             Slice<u8> output) noexcept;
inline constexpr u32 kGatewayUserMaximumTokenBytes = 2 * 1024;
inline constexpr u32 kGatewayUserMaximumOperatingSystemBytes = 32;
inline constexpr u32 kGatewayUserMaximumBrowserBytes = 64;
inline constexpr u32 kGatewayUserMaximumLocaleBytes = 32;
inline constexpr u32 kGatewayUserMaximumUserAgentBytes = 512;
inline constexpr u32 kGatewayUserMaximumVersionBytes = 64;
inline constexpr u32 kGatewayUserMaximumReleaseChannelBytes = 32;
inline constexpr u32 kGatewayUserMaximumArchitectureBytes = 16;
inline constexpr u32 kGatewayUserLaunchIdBytes = 36;

enum class GatewayUserStatus : u8 {
    online,
    idle,
    dnd,
    invisible,
};
// Gateway/settings wire name; empty for an unknown value.
constexpr Utf8View gateway_user_status_name(GatewayUserStatus status) noexcept {
    switch (status) {
    case GatewayUserStatus::online:
        return utf8_literal("online");
    case GatewayUserStatus::idle:
        return utf8_literal("idle");
    case GatewayUserStatus::dnd:
        return utf8_literal("dnd");
    case GatewayUserStatus::invisible:
        return utf8_literal("invisible");
    default:
        return {};
    }
}

// The client properties a user session reports, in the order the Windows
// desktop client sends them. Identify and the X-Super-Properties header carry
// the same object.
struct GatewayUserProperties {
    Utf8View operating_system;
    Utf8View browser;
    Utf8View release_channel;
    Utf8View client_version;
    Utf8View os_version;
    Utf8View os_arch;
    Utf8View app_arch;
    Utf8View system_locale;
    Utf8View client_launch_id;
    Utf8View browser_user_agent;
    Utf8View browser_version;
    Utf8View os_sdk_version;
    u32 client_build_number;
};

// Writes the properties object, or with a null output measures it. Every
// string must be non-empty, valid UTF-8 and within its limit, the launch ID a
// UUID and the build number set. A short output gets output_too_small and no
// bytes.
GatewayCommandWriteResult gateway_write_user_properties(const GatewayUserProperties& properties,
                                                        Slice<u8> output) noexcept;

// Normal-user Gateway v9 fresh-session Identify. It has no application intents/shard
// tuple and claims no cached versions. Use the same client compatibility
// properties as REST; client_build_number describes that protocol profile,
// independently of MaiCord's binary version.
struct GatewayUserIdentify {
    Utf8View token;
    GatewayUserProperties properties;
    u64 since_ms;
    GatewayUserStatus status;
    bool has_since;
    bool afk;
};

GatewayCommandWriteResult gateway_write_user_identify(const GatewayUserIdentify& identify,
                                                      Slice<u8> output) noexcept;
GatewayCommandWriteResult gateway_write_resume(const GatewayResume& resume, Slice<u8> output) noexcept;
GatewayCommandWriteResult gateway_write_guild_subscriptions_bulk(
    const GatewayGuildSubscription& subscription, Slice<u8> output) noexcept;

} // namespace maicord
