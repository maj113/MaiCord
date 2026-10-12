#pragma once

#include "maicord/protocol/rest.hpp"

namespace maicord {

// Community features: membership screening (the server rules form), the
// onboarding questions and scheduled events. Every request runs on the
// DETAILS lane through one account entry point; bodies are built here so the
// executable and the tests share one serializer, and the module only checks
// that a body is a bounded JSON object before sending it.

inline constexpr u64 kGuildVerificationGetRouteId = 0x4D56455246474554ull;
inline constexpr u64 kGuildVerificationSubmitRouteId = 0x4D56455246505554ull;
inline constexpr u64 kGuildOnboardingGetRouteId = 0x4F4E424F41524447ull;
inline constexpr u64 kGuildOnboardingSubmitRouteId = 0x4F4E424F41525350ull;
inline constexpr u64 kGuildEventsListRouteId = 0x4556454E544C5354ull;
inline constexpr u64 kGuildEventGetRouteId = 0x4556454E54474554ull;
inline constexpr u64 kGuildEventCreateRouteId = 0x4556454E54435254ull;
inline constexpr u64 kGuildEventInterestRouteId = 0x4556454E54525356ull;
inline constexpr u64 kGuildEventRsvpsRouteId = 0x4556454E54534D45ull;
inline constexpr u64 kGuildProfileGetRouteId = 0x4750524F46474554ull;
inline constexpr u64 kGuildJoinRouteId = 0x474A4F494E4D4521ull;

inline constexpr u32 kGuildCommunityMaximumTargetBytes = 160;
inline constexpr u32 kGuildCommunityMaximumBodyBytes = 48 * 1024;
inline constexpr u32 kGuildCommunityMaximumResponseBytes = 512 * 1024;

enum class GuildCommunityOperation : u8 {
    verification_get,    // GET    /guilds/{g}/member-verification?with_guild=false
    verification_submit, // PUT    /guilds/{g}/requests/@me            {version, form_fields}
    onboarding_get,      // GET    /guilds/{g}/onboarding
    onboarding_submit,   // POST   /guilds/{g}/onboarding-responses    {onboarding_responses, ..._seen}
    events_list,         // GET    /guilds/{g}/scheduled-events?with_user_count=true
    event_get,           // GET    /guilds/{g}/scheduled-events/{e}?with_user_count=true
    event_create,        // POST   /guilds/{g}/scheduled-events
    event_interested,    // PUT    /guilds/{g}/scheduled-events/{e}/users/@me  {"response":1}
    event_uninterested,  // DELETE /guilds/{g}/scheduled-events/{e}/users/@me
    event_rsvps,         // GET    /users/@me/scheduled-events?guild_ids={g}
    onboarding_update,   // PUT    /guilds/{g}/onboarding-responses    (Channels & Roles)
    guild_profile_get,   // GET    /guilds/{g}/profile                 (server tag popout)
    guild_join,          // PUT    /guilds/{g}/members/@me?lurker=true&session_id={s}  {}
    count,
};

// Body bytes are borrowed for the call only; the REST client copies them.
struct GuildCommunityRequest {
    u64 guild_id;
    u64 event_id;
    const u8* body;
    u32 body_bytes;
    GuildCommunityOperation operation;
    u8 reserved[3];
};
static_assert(sizeof(GuildCommunityRequest) == 32);

bool guild_community_request_valid(const GuildCommunityRequest&) noexcept;
bool guild_community_operation_has_body(GuildCommunityOperation) noexcept;

enum class GuildCommunityWriteError : u8 { none, invalid_input, output_capacity };
struct GuildCommunityWriteResult {
    u64 route_id;
    u32 target_bytes;
    RestMethod method;
    GuildCommunityWriteError error;
    constexpr bool ok() const noexcept { return error == GuildCommunityWriteError::none; }
};
// Writes the target ("/api/v10/...") for a valid request. The body is the
// request's own (checked by guild_community_request_valid) except for
// event_interested, whose fixed body is written to `body`. session_id (the
// Gateway session, alphanumeric) is guild_join's: the lurked server's
// GUILD_CREATE goes to that session; empty omits it.
GuildCommunityWriteResult guild_community_write_target(const GuildCommunityRequest&, Slice<u8> target,
                                                       Utf8View session_id = {}) noexcept;
// {"response":1}; returns the bytes written or 0.
u32 guild_community_write_interest_body(Slice<u8> body) noexcept;
// The fixed body of guild_join ({}); returns the bytes written or 0.
u32 guild_community_write_join_body(Slice<u8> body) noexcept;
// guild_join's X-Context-Properties line: base64 of an empty object (e30=).
// Returns the bytes written (rest_append_extra_header) or 0.
u32 guild_community_write_join_context(Slice<u8> output) noexcept;

enum class GuildCommunityOutcome : u8 {
    ok,
    not_found,    // 404 / unknown guild, event or form
    forbidden,    // 403
    rate_limited, // 429
    invalid,      // 400 (the body names the problem)
    unavailable,  // other statuses and transport failures
    malformed,
};
GuildCommunityOutcome guild_community_classify(u16 http_status) noexcept;

// Membership screening

inline constexpr u32 kGuildVerificationFieldMaximum = 8;
inline constexpr u32 kGuildVerificationValueMaximum = 32;
inline constexpr u32 kGuildVerificationTextBytes = 12 * 1024;
inline constexpr u32 kGuildVerificationVersionBytes = 48;

enum class GuildVerificationFieldType : u8 { unknown, terms, text_input, paragraph, multiple_choice, verification };
struct GuildVerificationText {
    u16 offset, bytes;
};
struct GuildVerificationField {
    GuildVerificationText label, description, placeholder;
    // TERMS: the rules; MULTIPLE_CHOICE: the choices.
    GuildVerificationText values[kGuildVerificationValueMaximum];
    u8 value_count;
    GuildVerificationFieldType type;
    bool required;
};
struct GuildVerificationForm {
    GuildVerificationField fields[kGuildVerificationFieldMaximum];
    GuildVerificationText description;
    u8 field_count;
    u8 version_bytes;
    bool truncated; // text or fields beyond the bounds were dropped
    char version[kGuildVerificationVersionBytes];
    char text[kGuildVerificationTextBytes];
    u32 text_used;
};
inline Utf8View guild_verification_text(const GuildVerificationForm& form, GuildVerificationText text) noexcept {
    return {form.text + text.offset, text.bytes};
}
// Decodes GET /guilds/{g}/member-verification. False for a body that is not a
// form object (fields of an unknown type decode as unknown).
bool guild_verification_decode(Utf8View body, Slice<u32> scratch, GuildVerificationForm* output) noexcept;
// Whether the form can be answered here: no required field of an unknown type.
bool guild_verification_supported(const GuildVerificationForm&) noexcept;

// One answer per decoded field, in field order.
struct GuildVerificationAnswer {
    Utf8View text; // TEXT_INPUT / PARAGRAPH
    u32 choice;    // MULTIPLE_CHOICE index
    bool agreed;   // TERMS / VERIFICATION
    bool answered;
};
// Required answers present: terms agreed, choices in range, text non-blank.
bool guild_verification_answers_complete(const GuildVerificationForm&, ConstSlice<GuildVerificationAnswer>) noexcept;
// PUT body: {"version":...,"form_fields":[<each original field object plus
// "response">]}. The original form body is re-read so every field is echoed
// exactly as the server sent it. Returns the bytes written, 0 on failure.
u32 guild_verification_write_submit(Utf8View form_body, ConstSlice<GuildVerificationAnswer> answers,
                                    Slice<u32> scratch, Slice<u8> output) noexcept;
enum class GuildApplicationStatus : u8 { unknown, started, submitted, rejected, approved };
// The join request returned by the PUT.
GuildApplicationStatus guild_verification_decode_status(Utf8View body, Slice<u32> scratch) noexcept;

// Onboarding

inline constexpr u32 kGuildOnboardingPromptMaximum = 16;
inline constexpr u32 kGuildOnboardingOptionMaximum = 128;
inline constexpr u32 kGuildOnboardingDefaultChannelMaximum = 32;
inline constexpr u32 kGuildOnboardingTextBytes = 16 * 1024;

struct GuildOnboardingOption {
    u64 id;
    GuildVerificationText title, description, emoji;
    u64 emoji_id;
    u16 role_count, channel_count;
    bool selected; // in the member's saved responses
};
struct GuildOnboardingPrompt {
    u64 id;
    GuildVerificationText title;
    u16 first_option, option_count;
    u8 type; // 0 multiple choice, 1 dropdown
    bool single_select, required, in_onboarding;
};
struct GuildOnboarding {
    GuildOnboardingPrompt prompts[kGuildOnboardingPromptMaximum];
    GuildOnboardingOption options[kGuildOnboardingOptionMaximum];
    u64 default_channels[kGuildOnboardingDefaultChannelMaximum];
    u32 prompt_count, option_count, default_channel_count;
    u8 mode;
    bool enabled, truncated;
    char text[kGuildOnboardingTextBytes];
    u32 text_used;
};
inline Utf8View guild_onboarding_text(const GuildOnboarding& onboarding, GuildVerificationText text) noexcept {
    return {onboarding.text + text.offset, text.bytes};
}
bool guild_onboarding_decode(Utf8View body, Slice<u32> scratch, GuildOnboarding* output) noexcept;
// Prompts shown in the onboarding flow (in_onboarding), in order.
u32 guild_onboarding_flow_prompts(const GuildOnboarding&, Slice<u16> output) noexcept;
// Selection rules: single-select prompts keep one option. Returns whether
// the selection changed.
bool guild_onboarding_toggle(GuildOnboarding*, u32 prompt, u32 option) noexcept;
bool guild_onboarding_prompt_answered(const GuildOnboarding&, u32 prompt) noexcept;
// POST body with the selected options and every flow prompt/option as seen
// at `seen_ms`. Returns the bytes written, 0 on failure.
u32 guild_onboarding_write_responses(const GuildOnboarding&, u64 seen_ms, Slice<u8> output) noexcept;

// Scheduled events

inline constexpr u32 kScheduledEventNameBytes = 256;
inline constexpr u32 kScheduledEventDescriptionBytes = 1024;
inline constexpr u32 kScheduledEventLocationBytes = 128;
inline constexpr u32 kScheduledEventImageBytes = 48;

enum class ScheduledEventStatus : u8 { unknown, scheduled, active, completed, canceled };
enum class ScheduledEventEntity : u8 { unknown, stage, voice, external };

struct ScheduledEvent {
    u64 id, guild_id, channel_id, creator_id;
    u64 start_ms, end_ms; // Unix milliseconds; end 0 when open-ended
    u32 user_count;
    ScheduledEventStatus status;
    ScheduledEventEntity entity;
    bool user_count_known;
    bool interested; // the current account marked it Interested
    bool interested_known;
    u8 image_bytes;
    u8 location_bytes;
    u16 name_bytes;
    u16 description_bytes;
    char name[kScheduledEventNameBytes];
    char description[kScheduledEventDescriptionBytes];
    char location[kScheduledEventLocationBytes];
    char image[kScheduledEventImageBytes];
};
// Decodes one event object (REST or a GUILD_SCHEDULED_EVENT_* dispatch).
bool scheduled_event_decode(Utf8View object, Slice<u32> scratch, ScheduledEvent* output) noexcept;
using ScheduledEventVisitor = bool (*)(void* context, const ScheduledEvent&) noexcept;
// Decodes an array of events (list response or guild_scheduled_events);
// malformed rows are skipped. Returns the number visited, ~0u on failure.
u32 scheduled_event_decode_list(Utf8View array, Slice<u32> scratch, ScheduledEventVisitor, void* context) noexcept;
struct ScheduledEventUser {
    u64 event_id, guild_id, user_id;
};
// GUILD_SCHEDULED_EVENT_USER_ADD/REMOVE and /users/@me/scheduled-events rows.
bool scheduled_event_decode_user(Utf8View object, Slice<u32> scratch, ScheduledEventUser* output) noexcept;
using ScheduledEventUserVisitor = bool (*)(void* context, const ScheduledEventUser&) noexcept;
u32 scheduled_event_decode_users(Utf8View array, Slice<u32> scratch, ScheduledEventUserVisitor,
                                 void* context) noexcept;

struct ScheduledEventCreate {
    Utf8View name, description, location;
    u64 channel_id;
    u64 start_unix_seconds, end_unix_seconds; // end 0: none (required for external)
    ScheduledEventEntity entity;
};
inline constexpr u32 kScheduledEventNameMaximum = 100;        // code points
inline constexpr u32 kScheduledEventDescriptionMaximum = 1000; // code points
inline constexpr u32 kScheduledEventLocationMaximum = 100;     // code points
enum class ScheduledEventCreateError : u8 {
    none,
    name,     // empty or too long
    description,
    location, // external events need one
    channel,  // stage/voice events need one
    start,    // not in the future
    end,      // external events need an end after the start
};
ScheduledEventCreateError scheduled_event_create_validate(const ScheduledEventCreate&, u64 now_unix_seconds) noexcept;
// POST body; privacy_level 2 (server only). Returns the bytes written or 0.
u32 scheduled_event_write_create(const ScheduledEventCreate&, Slice<u8> output) noexcept;
// https://discord.com/events/{guild}/{event}
u32 scheduled_event_write_link(u64 guild_id, u64 event_id, Slice<char> output) noexcept;

// Server profile (the server tag popout)

inline constexpr u32 kGuildProfileNameBytes = 256;
inline constexpr u32 kGuildProfileDescriptionBytes = 1024;
inline constexpr u32 kGuildProfileHashBytes = 64;
inline constexpr u32 kGuildProfileTagBytes = 32;
inline constexpr u32 kGuildProfileTraits = 5;
inline constexpr u32 kGuildProfileTraitBytes = 96;
// The API error code for a profile the server limits ("Private Server").
inline constexpr u64 kGuildProfilePrivateErrorCode = 50001;

enum GuildProfileFeature : u32 {
    guild_profile_discoverable = 1u << 0,      // DISCOVERABLE: joinable without an invite
    guild_profile_tags = 1u << 1,              // GUILD_TAGS
    guild_profile_verification_gate = 1u << 2, // MEMBER_VERIFICATION_GATE_ENABLED
    guild_profile_manual_approval = 1u << 3,   // MEMBER_VERIFICATION_MANUAL_APPROVAL
    guild_profile_verified = 1u << 4,          // VERIFIED
    guild_profile_partnered = 1u << 5,         // PARTNERED
};
enum class GuildProfileVisibility : u8 { not_specified, public_, restricted, public_with_recruitment };
struct GuildProfileTrait {
    u8 label_bytes;
    char label[kGuildProfileTraitBytes];
};
struct GuildProfile {
    u64 id;
    u32 online_count, member_count;
    u32 brand_color; // 0xRRGGBB when has_brand_color
    u32 features;    // GuildProfileFeature
    GuildProfileVisibility visibility;
    bool has_brand_color;
    u8 icon_bytes, banner_bytes, badge_bytes, tag_bytes, trait_count;
    u16 name_bytes, description_bytes;
    char name[kGuildProfileNameBytes];
    char description[kGuildProfileDescriptionBytes];
    char icon[kGuildProfileHashBytes];   // icon_hash
    char banner[kGuildProfileHashBytes]; // custom_banner_hash
    char badge[kGuildProfileHashBytes];  // badge_hash (the tag's badge image)
    char tag[kGuildProfileTagBytes];
    GuildProfileTrait traits[kGuildProfileTraits]; // in position order, empty slots skipped
};
// Decodes GET /guilds/{g}/profile. False without an id or name, or for a body
// that is not an object; unknown members are ignored and long text truncates.
bool guild_profile_decode(Utf8View body, Slice<u32> scratch, GuildProfile* output) noexcept;

} // namespace maicord
