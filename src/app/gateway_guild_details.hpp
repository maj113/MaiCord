#pragma once

#include "maicord/app/gateway_client.hpp"

#include "gateway_payload_writer.hpp"

namespace maicord {

// Guild detail keys seen on one state object. Kept raw while the object is
// visited; only guild create/update records decode and write them.
struct GatewayGuildDetailValues {
    JsonValue description;
    JsonValue vanity_url_code;
    JsonValue incidents_data;
    JsonValue splash;
    u64 rules_channel_id;
    u64 public_updates_channel_id;
    u64 safety_alerts_channel_id;
    u32 system_channel_flags;
    u32 premium_subscription_count;
    u32 member_count;
    // GuildDetailField bits of keys present with a usable value.
    u16 seen;
    u8 premium_tier;
    bool premium_progress_bar_enabled;
};

// True when `name` is a detail key. Details are advisory: a malformed value
// leaves the field unseen and a repeated key keeps the first value.
bool gateway_guild_detail_visit(GatewayGuildDetailValues* values, JsonString name, JsonValue value) noexcept;
// Fills the fields `to` has not seen from `from` (user-account properties).
void gateway_guild_detail_merge(GatewayGuildDetailValues* to, const GatewayGuildDetailValues& from) noexcept;
// Appends the details record of one guild create/update. Every field but
// the boost and member counts counts as observed, absent ones as zero or
// null, since such a record replaces the guild. A description or feature
// list that cannot be carried leaves its bit clear. False only when the
// writer is full; *bytes is the record size.
bool gateway_guild_details_append(const GatewayGuildDetailValues& values, JsonValue features, bool features_seen,
                                  Slice<u32> scratch, u32 maximum_depth, GatewayPayloadWriter* writer,
                                  u16* bytes) noexcept;

} // namespace maicord
