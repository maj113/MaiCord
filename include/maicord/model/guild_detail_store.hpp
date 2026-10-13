#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {

// Server details that GuildRecord does not hold, kept per guild beside the
// chat state: boost state, system-message flags, community channels, the
// description and vanity code, incident times and features.

// Fields observed for a guild; only these replace stored values.
enum GuildDetailField : u16 {
    guild_detail_premium_tier = 1u << 0,
    guild_detail_boost_count = 1u << 1,
    guild_detail_system_channel_flags = 1u << 2,
    guild_detail_rules_channel = 1u << 3,
    guild_detail_public_updates_channel = 1u << 4,
    guild_detail_safety_alerts_channel = 1u << 5,
    guild_detail_progress_bar = 1u << 6,
    guild_detail_description = 1u << 7,
    guild_detail_vanity = 1u << 8,
    guild_detail_incidents = 1u << 9,
    // The complete features array; every name is carried.
    guild_detail_features = 1u << 10,
    guild_detail_member_count = 1u << 11,
    // The invite splash hash.
    guild_detail_splash = 1u << 12,
    guild_detail_all = (1u << 13) - 1,
};

// Features Server Settings reads, decoded from the feature names. Names
// without a bit are still kept verbatim where the raw list is retained.
enum GuildFeature : u64 {
    guild_feature_more_emoji = 1ull << 0,
    guild_feature_more_stickers = 1ull << 1,
    guild_feature_role_icons = 1ull << 2,
    guild_feature_banner = 1ull << 3,
    guild_feature_animated_icon = 1ull << 4,
    guild_feature_animated_banner = 1ull << 5,
    guild_feature_community = 1ull << 6,
    guild_feature_activity_feed_enabled = 1ull << 7,
    guild_feature_activity_feed_disabled = 1ull << 8,
    guild_feature_vanity_url = 1ull << 9,
    guild_feature_invite_splash = 1ull << 10,
    guild_feature_discoverable = 1ull << 11,
    guild_feature_invites_disabled = 1ull << 12,
    guild_feature_raid_alerts_disabled = 1ull << 13,
    guild_feature_non_community_raid_alerts = 1ull << 14,
    guild_feature_prune_requires_admin = 1ull << 15,
    guild_feature_manual_member_approval = 1ull << 16,
    guild_feature_moderation_experience = 1ull << 17,
    guild_feature_creator_monetizable = 1ull << 18,
    guild_feature_creator_monetizable_provisional = 1ull << 19,
    guild_feature_creator_monetizable_disabled = 1ull << 20,
    guild_feature_enhanced_role_colors = 1ull << 21,
    guild_feature_onboarding = 1ull << 22,
    guild_feature_server_guide = 1ull << 23,
    guild_feature_summaries_ga = 1ull << 24,
    guild_feature_summaries_enabled = 1ull << 25,
};
// Zero for names Server Settings does not act on.
u64 guild_feature_bit(Utf8View name) noexcept;
// The canonical name of one guild_feature_* bit; empty for anything else.
Utf8View guild_feature_name(u64 bit) noexcept;
// Walks a run of feature names, each one length byte and its bytes. False
// at the end or at a malformed entry.
bool guild_feature_name_next(ConstSlice<u8> names, u32* cursor, Utf8View* name) noexcept;

inline constexpr u32 kGuildDetailDescriptionMaximumBytes = 1'200;
inline constexpr u32 kGuildDetailVanityMaximumBytes = 64;
// An image hash: 32 hex characters, "a_" first when animated.
inline constexpr u32 kGuildDetailHashBytes = 34;

struct GuildDetailRecord {
    u64 guild_id;
    u64 rules_channel_id;
    u64 public_updates_channel_id;
    u64 safety_alerts_channel_id;
    // Unix milliseconds; zero when there is no incident time.
    u64 invites_disabled_until_ms;
    u64 dms_disabled_until_ms;
    u64 raid_detected_at_ms;
    u64 dm_spam_detected_at_ms;
    u64 features;
    u32 system_channel_flags;
    u32 premium_subscription_count;
    // The guild's member_count when it was created or joined.
    u32 member_count;
    // Text bank region: description, vanity code, then the retained names.
    u32 text_offset;
    u16 description_bytes;
    u16 feature_names_bytes;
    u16 feature_count;
    u16 known;
    u8 vanity_bytes;
    u8 premium_tier;
    u8 lockdown_duration_hours;
    bool premium_progress_bar_enabled;
    // The names below are the server's complete list in its order.
    bool feature_names_kept;
    u8 splash_bytes;
    char splash[kGuildDetailHashBytes];
};

struct GuildDetailStore {
    GuildDetailRecord* records;
    char* text;
    u32 capacity;
    u32 count;
    u32 text_capacity;
    u32 text_used;
    // Advances on every change.
    u32 revision;
};

// One guild's observation. Views are borrowed for the call. Feature names
// replace the stored run only when keep_feature_names is set; otherwise the
// stored run is dropped while the feature bits still update.
struct GuildDetailInput {
    u64 guild_id;
    u64 rules_channel_id;
    u64 public_updates_channel_id;
    u64 safety_alerts_channel_id;
    u64 invites_disabled_until_ms;
    u64 dms_disabled_until_ms;
    u64 raid_detected_at_ms;
    u64 dm_spam_detected_at_ms;
    Utf8View description;
    Utf8View vanity_url_code;
    // Empty for none.
    Utf8View splash;
    ConstSlice<u8> feature_names;
    u32 system_channel_flags;
    u32 premium_subscription_count;
    u32 member_count;
    u16 feature_count;
    u16 known;
    u8 premium_tier;
    u8 lockdown_duration_hours;
    bool premium_progress_bar_enabled;
    bool keep_feature_names;
};

bool guild_detail_store_bind(GuildDetailStore* store, Slice<GuildDetailRecord> records, Slice<char> text) noexcept;
void guild_detail_store_clear(GuildDetailStore* store) noexcept;
// False for invalid input or a full record table. When the text bank cannot
// hold everything even after compaction, the retained names go first, then
// the description; their known bits are cleared.
bool guild_detail_store_apply(GuildDetailStore* store, const GuildDetailInput& input) noexcept;
bool guild_detail_store_remove(GuildDetailStore* store, u64 guild_id) noexcept;
const GuildDetailRecord* guild_detail_store_find(const GuildDetailStore& store, u64 guild_id) noexcept;
Utf8View guild_detail_store_description(const GuildDetailStore& store, const GuildDetailRecord& record) noexcept;
Utf8View guild_detail_store_vanity_url_code(const GuildDetailStore& store, const GuildDetailRecord& record) noexcept;
inline Utf8View guild_detail_store_splash(const GuildDetailRecord& record) noexcept {
    return {record.splash, record.splash_bytes};
}
// Empty unless feature_names_kept.
ConstSlice<u8> guild_detail_store_feature_names(const GuildDetailStore& store, const GuildDetailRecord& record) noexcept;

} // namespace maicord
