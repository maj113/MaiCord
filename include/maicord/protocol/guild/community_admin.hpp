#pragma once

#include "maicord/protocol/guild/community.hpp"

namespace maicord {

// Server Settings' Access and Server Template pages on the community lane
// (GuildCommunityOperation verification_update through discovery_categories):
// the bodies they send and the replies they read. Writers return the bytes
// written, 0 for invalid input or short output; decoders are false (or ~0u)
// for a body they cannot read and leave the output untouched then.

// Membership screening, as the Access page edits it ------------------------

inline constexpr u32 kGuildVerificationEditFieldMaximum = 5;
inline constexpr u32 kGuildVerificationRuleMaximum = 16;
inline constexpr u32 kGuildVerificationChoiceMaximum = 8;
// A question's label and a rule; a multiple-choice answer.
inline constexpr u32 kGuildVerificationLabelCodepoints = 300;
inline constexpr u32 kGuildVerificationChoiceCodepoints = 150;

struct GuildVerificationFieldWrite {
    Utf8View label, description, placeholder;
    // TERMS: the rules; MULTIPLE_CHOICE: the choices; empty for the text answers.
    ConstSlice<Utf8View> values;
    GuildVerificationFieldType type;
    bool required;
};
// What a gate being turned off does with the applications still waiting.
enum class GuildVerificationBulkAction : u8 { none, approved, rejected };
// At most five fields of the four editable types, one TERMS at most, labels
// and values in their limits.
bool guild_verification_fields_valid(ConstSlice<GuildVerificationFieldWrite> fields) noexcept;
// PATCH /guilds/{g}/member-verification as the official client sends it:
// {"form_fields":[{"field_type","label","values"|"choices","required"[,"description"][,"placeholder"]}...],
// "enabled":b[,"bulk_action":"APPROVED"|"REJECTED"]}.
u32 guild_verification_write_update(ConstSlice<GuildVerificationFieldWrite> fields, bool enabled,
                                    GuildVerificationBulkAction bulk_action, Slice<u8> output) noexcept;

// Server templates ---------------------------------------------------------

inline constexpr u32 kGuildTemplateCodeBytes = 32;
inline constexpr u32 kGuildTemplateNameCodepoints = 100;
inline constexpr u32 kGuildTemplateDescriptionCodepoints = 120;
// What the preview lists: the snapshot's channels and roles as lines of text.
inline constexpr u32 kGuildTemplatePreviewBytes = 2048;

struct GuildTemplate {
    // Unix milliseconds; zero when absent.
    u64 created_at_ms, updated_at_ms;
    u32 usage_count;
    u16 name_bytes, description_bytes;
    u8 code_bytes;
    // is_dirty: the server changed since the last sync.
    bool dirty;
    char code[kGuildTemplateCodeBytes];
    char name[kGuildTemplateNameCodepoints * 4];
    char description[kGuildTemplateDescriptionCodepoints * 4];
    // The snapshot's channels in order ("# general", "🔊 General" under their
    // category) then its roles, a line each; cut at a line that does not fit.
    u16 preview_bytes;
    u16 channel_count, role_count;
    char preview[kGuildTemplatePreviewBytes];
};
// Letters and digits, 1 to 32 of them.
bool guild_template_code_valid(Utf8View code) noexcept;
// The official form's checks: a title of at least two characters once trimmed
// and at most 100, a description of at most 120.
bool guild_template_title_valid(Utf8View name) noexcept;
bool guild_template_description_valid(Utf8View description) noexcept;
// Create and modify: {"name":"...","description":"..."}, the texts as typed.
u32 guild_template_write(Utf8View name, Utf8View description, Slice<u8> output) noexcept;
// One template object (the create, sync and modify replies).
bool guild_template_decode(Utf8View body, Slice<u32> scratch, GuildTemplate* output) noexcept;
// GET /guilds/{g}/templates: the number of templates, the first decoded into
// `first` (a server has at most one); ~0u for a body that is not a list.
u32 guild_template_decode_list(Utf8View body, Slice<u32> scratch, GuildTemplate* first) noexcept;
// The link the page copies: https://discord.new/{code}.
u32 guild_template_write_link(Utf8View code, Slice<char> output) noexcept;

// The `total` of a GET /guilds/{g}/requests reply (the applications in the asked status); ~0u when unreadable.
u32 guild_join_requests_decode_total(Utf8View body, Slice<u32> scratch) noexcept;

// Server Discovery ---------------------------------------------------------

// GET /guilds/{g}/discovery-requirements. Exempt servers (verified ones)
// carry only the two `sufficient` keys; `checklist` is false then.
struct GuildDiscoveryRequirements {
    // grace_period_end_date, Unix milliseconds; zero when absent.
    u64 grace_period_end_ms;
    u32 minimum_size;
    u32 minimum_age_days;
    bool sufficient, sufficient_without_grace_period;
    bool checklist;
    bool safe_environment, size, age, healthy, health_score_pending, protected_moderation;
    // Disallowed words in the name, description or channel names.
    bool bad_content;
};
bool guild_discovery_requirements_decode(Utf8View body, Slice<u32> scratch,
                                         GuildDiscoveryRequirements* output) noexcept;

inline constexpr u32 kGuildDiscoveryKeywordMaximum = 10;
inline constexpr u32 kGuildDiscoveryKeywordCodepoints = 30;
inline constexpr u32 kGuildDiscoveryKeywordBytes = kGuildDiscoveryKeywordCodepoints * 4;
// The metadata read must stay whole for its PATCH (which replaces every key).
inline constexpr u32 kGuildDiscoveryMetadataMaximumBytes = 32 * 1024;

inline constexpr u32 kGuildDiscoverySubcategoryMaximum = 5;
struct GuildDiscoveryMetadata {
    u32 primary_category_id;
    u8 keyword_count;
    bool emoji_discoverability_enabled;
    u8 keyword_bytes[kGuildDiscoveryKeywordMaximum];
    char keywords[kGuildDiscoveryKeywordMaximum][kGuildDiscoveryKeywordBytes];
    // category_ids: the subcategories, added and removed on their own routes.
    u32 subcategories[kGuildDiscoverySubcategoryMaximum];
    u8 subcategory_count;
};
// GET /guilds/{g}/discovery-metadata.
bool guild_discovery_metadata_decode(Utf8View body, Slice<u32> scratch, GuildDiscoveryMetadata* output) noexcept;
// PATCH /guilds/{g}/discovery-metadata as the official client builds it from
// its read: primary_category_id, emoji_discoverability_enabled,
// partner_actioned_timestamp, partner_application_timestamp, keywords,
// is_published, reasons_to_join, social_links, about, in that order. The
// three edited keys take the given values; the others repeat the read
// verbatim, and a key the read lacked is left out.
u32 guild_discovery_metadata_write(Utf8View read_body, Slice<u32> scratch, u32 primary_category_id,
                                   ConstSlice<Utf8View> keywords, bool emoji_discoverability_enabled,
                                   Slice<u8> output) noexcept;

inline constexpr u32 kDiscoveryCategoryNameBytes = 64;
struct GuildDiscoveryCategory {
    u32 id;
    u8 name_bytes;
    bool primary;
    char name[kDiscoveryCategoryNameBytes];
};
// GET /discovery/categories: every category in the order listed, `primary`
// marking those a server may take as its primary one; the count or ~0u.
u32 guild_discovery_decode_categories(Utf8View body, Slice<u32> scratch,
                                      Slice<GuildDiscoveryCategory> output) noexcept;

// Apply from Profile: PATCH /guilds/{g}/profile {"visibility":n} (1 public,
// 3 public with recruitment).
u32 guild_visibility_write(u8 visibility, Slice<u8> output) noexcept;

} // namespace maicord
