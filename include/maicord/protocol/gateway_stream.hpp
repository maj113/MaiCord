#pragma once

#include "maicord/protocol/gateway_commands.hpp"
#include "maicord/protocol/gateway_events.hpp"

namespace maicord {

// Go Live (application streams) on the main Gateway.
// Commands: 18 STREAM_CREATE, 19 STREAM_DELETE, 20 STREAM_WATCH, 21 STREAM_PING,
// 22 STREAM_SET_PAUSED. Dispatches: STREAM_CREATE, STREAM_SERVER_UPDATE,
// STREAM_UPDATE, STREAM_DELETE.
enum class GatewayStreamType : u8 {
    none,
    guild, // "guild:<guild>:<channel>:<owner>"
    call,  // "call:<channel>:<owner>" (private call)
};
// Crosses the account-module ABI (stream_watch, stream snapshot).
struct GatewayStreamKey {
    u64 guild_id, channel_id, owner_id;
    GatewayStreamType type;
    u8 reserved[7];
    constexpr bool operator==(const GatewayStreamKey&) const noexcept = default;
};
static_assert(sizeof(GatewayStreamKey) == 32);
// "guild:" + three 20-digit snowflakes and separators.
inline constexpr u32 kGatewayStreamKeyMaximumBytes = 72;

bool gateway_stream_key_valid(const GatewayStreamKey& key) noexcept;
// The stream of owner_id in a voice channel; guild zero selects a private call.
GatewayStreamKey gateway_stream_key_for(u64 guild_id, u64 channel_id, u64 owner_id) noexcept;
// Writes the textual key; zero when invalid or the output is too small.
u32 gateway_stream_key_write(const GatewayStreamKey& key, Slice<char> output) noexcept;
// Strict: canonical decimal snowflakes, exact field count, known prefix.
bool gateway_stream_key_parse(Utf8View text, GatewayStreamKey* output) noexcept;

// Writers follow the Gateway command convention: an empty output is a size
// query (bytes = required, error output_too_small); a short output writes nothing.
GatewayCommandWriteResult gateway_write_stream_delete(const GatewayStreamKey& key, Slice<u8> output) noexcept;
GatewayCommandWriteResult gateway_write_stream_watch(const GatewayStreamKey& key, Slice<u8> output) noexcept;
GatewayCommandWriteResult gateway_write_stream_ping(const GatewayStreamKey& key, Slice<u8> output) noexcept;

enum class GatewayStreamDeleteReason : u8 {
    none, // Absent or null.
    user_requested,
    stream_full,
    unauthorized,
    safety_guild_rate_limited,
    other,
};
// Strings borrow the dispatch document. The stream token is a secret: never
// log, persist or retain it past the document.
struct GatewayStreamCreate {
    GatewayStreamKey key;
    u64 rtc_server_id;  // Zero when absent/null: no media server yet.
    u64 rtc_channel_id; // Zero when absent/null (the voice server uses rtc_server_id - 1).
    JsonString region;
    u32 viewer_count;
    bool paused;
};
struct GatewayStreamServer {
    GatewayStreamKey key;
    JsonString token, endpoint;
    bool unavailable; // Null endpoint: the media server is being reallocated.
};
struct GatewayStreamUpdate {
    GatewayStreamKey key;
    JsonString region;
    u32 viewer_count;
    bool paused;
    bool has_viewers, has_paused;
};
struct GatewayStreamDelete {
    GatewayStreamKey key;
    GatewayStreamDeleteReason reason;
    bool unavailable;
};
GatewayEventDecodeResult gateway_decode_stream_create(JsonValue data, Slice<u32> scratch, u32 maximum_depth,
                                                      GatewayStreamCreate* output) noexcept;
GatewayEventDecodeResult gateway_decode_stream_server(JsonValue data, Slice<u32> scratch, u32 maximum_depth,
                                                      GatewayStreamServer* output) noexcept;
GatewayEventDecodeResult gateway_decode_stream_update(JsonValue data, Slice<u32> scratch, u32 maximum_depth,
                                                      GatewayStreamUpdate* output) noexcept;
GatewayEventDecodeResult gateway_decode_stream_delete(JsonValue data, Slice<u32> scratch, u32 maximum_depth,
                                                      GatewayStreamDelete* output) noexcept;

// REST: GET /api/v9/streams/{key}/preview?version=<ms> returns {"url": "..."}.
// The preview image is not end-to-end encrypted; do not log its URL.
inline constexpr u64 kGatewayStreamPreviewRouteId = 0x53545250524556ull; // "STRPREV"
inline constexpr u32 kGatewayStreamPreviewMaximumTargetBytes = 160;
u32 gateway_stream_preview_target(const GatewayStreamKey& key, u64 version, Slice<u8> output) noexcept;
inline constexpr u32 kGatewayStreamPreviewMaximumUrlBytes = 2048;
// Decodes "url" into output; zero unless it is a non-empty https URL that fits.
u32 gateway_stream_preview_url(Utf8View body, Slice<u32> scratch, Slice<char> output) noexcept;

} // namespace maicord
