#pragma once

#include "maicord/protocol/gateway_events.hpp"

namespace maicord {

inline constexpr u32 kGatewayMessageContentMaximumBytes = 16 * 1024;
inline constexpr u32 kGatewayBulkDeleteMaximumIds = 100;
inline constexpr u32 kGatewayMessageMentionRoleMaximumIds = 100;
inline constexpr u32 kGatewayMessageMentionUserMaximum = 100;
// Guilds allow 250 roles; longer member lists are rejected.
inline constexpr u32 kGatewayMessageMemberRoleMaximumIds = 256;
inline constexpr u32 kGatewayMessageUserNameMaximumBytes = 128;
inline constexpr u32 kGatewayMessageAvatarHashMaximumBytes = 34;
inline constexpr u32 kGatewayMessageReactionMaximum = 20;
inline constexpr u32 kGatewayMessageAttachmentMaximum = 10;
inline constexpr u32 kGatewayMessageWaveformMaximumSamples = 256;
inline constexpr u32 kGatewayMessageWaveformMaximumBytes = 344;
inline constexpr u32 kGatewayMessageVoiceFlag = 8192;
inline constexpr u32 kGatewayMessageEmbedMaximum = 10;
inline constexpr u32 kGatewayMessageEmbedFieldMaximum = 64;
inline constexpr u32 kGatewayMessageStickerMaximum = 3;
inline constexpr u32 kGatewayMessageComponentMaximum = 40;

struct GatewayMessageNamedUser {
    u64 id;
    Utf8View name;
};

enum class GatewayMessageReferenceState : u8 {
    none,
    unknown,
    deleted,
    available,
};

enum GatewayMessageRichTruncation : u32 {
    gateway_message_rich_truncated_reactions = 1u << 0,
    gateway_message_rich_truncated_attachments = 1u << 1,
    gateway_message_rich_truncated_embeds = 1u << 2,
    gateway_message_rich_truncated_embed_fields = 1u << 3,
    gateway_message_rich_truncated_stickers = 1u << 4,
    gateway_message_rich_truncated_components = 1u << 5,
    gateway_message_rich_truncated_text = 1u << 6,
};

// What the referenced message carries besides its text, for the reply
// preview's placeholder and trailing icon.
enum GatewayMessageReferenceContent : u8 {
    gateway_reference_attachments = 1u << 0,
    gateway_reference_embeds = 1u << 1,
    gateway_reference_stickers = 1u << 2,
    // content holds poll.question.text (the message text itself was empty).
    gateway_reference_poll_question = 1u << 3,
    gateway_reference_voice_message = 1u << 4,
    gateway_reference_components_v2 = 1u << 5,
    gateway_reference_interaction = 1u << 6,
    // The replying message mentions the quoted author (a reply with ping on): the preview names them "@Name".
    gateway_reference_mentions_author = 1u << 7,
};

// More of the quoted message the preview shows (GatewayMessageReference::quote_flags).
enum GatewayMessageReferenceQuote : u8 {
    // The quoted message is itself a forward: its preview leads with the forward icon.
    gateway_quote_forwarded = 1u << 0,
    // The reply carries media_mention: the preview shows the clip's title and "@time"
    // (reference.content holds "<title>\x1f@<time>").
    gateway_quote_media_mention = 1u << 1,
    // The mentioned attachment is a clip.
    gateway_quote_media_clip = 1u << 2,
};

struct GatewayMessageReference {
    u64 message_id;
    u64 channel_id;
    u64 guild_id;
    u64 author_id;
    Utf8View author_name;
    Utf8View author_avatar_hash;
    Utf8View content;
    GatewayMessageReferenceState state;
    u8 type;
    u8 content_flags;  // GatewayMessageReferenceContent
    u8 message_type;   // referenced message type (all known types are < 256)
    u8 quote_flags;    // GatewayMessageReferenceQuote
};

struct GatewayMessageReaction {
    u64 emoji_id;
    Utf8View name;
    u32 count;
    u32 normal_count;
    u32 burst_count;
    bool me;
    bool me_burst;
    bool animated;
};

// Command attribution is not a message reference. The header comes from
// interaction.user and name_localized ?? name.
struct GatewayMessageInteraction {
    u64 id;
    u64 user_id;
    Utf8View user_name;
    Utf8View user_avatar_hash;
    Utf8View command_name;
    u8 type;
};

bool gateway_message_waveform_valid(Utf8View waveform) noexcept;

struct GatewayMessageAttachment {
    u64 id;
    u64 size;
    Utf8View filename;
    Utf8View title;
    Utf8View description;
    Utf8View content_type;
    Utf8View url;
    u32 width;
    u32 height;
    u32 flags;
    bool has_width;
    bool has_height;
    bool ephemeral;
    // Decoder-owned text scratch; copied into the rich store before reuse.
    Utf8View waveform;
    u32 duration_ms;
    bool has_duration;
};

enum class GatewayMessageEmbedType : u8 {
    unknown,
    rich,
    image,
    video,
    gifv,
    article,
    link,
    poll_result,
    // Wire type "components": its components[] container is decoded into
    // description/color/media.
    components,
};

struct GatewayMessageEmbedField {
    Utf8View name;
    Utf8View value;
    u16 embed_index;
    bool inline_field;
};

// An embed carries independent image, thumbnail and video objects. All three
// descriptors are kept: choosing one to present is UI policy and must not
// destroy protocol data.
struct GatewayMessageEmbedMedia {
    Utf8View url;
    Utf8View proxy_url;
    Utf8View content_type;
    Utf8View description;
    u32 width;
    u32 height;
    u32 flags;
    bool has_width;
    bool has_height;
};

struct GatewayMessageEmbed {
    Utf8View title;
    Utf8View description;
    Utf8View url;
    Utf8View author_name;
    Utf8View footer_text;
    // author.url, author/footer proxy_icon_url (icons are fetched only through
    // the media proxy) and provider.name/url.
    Utf8View author_url;
    Utf8View author_icon;
    Utf8View footer_icon;
    Utf8View provider_name;
    Utf8View provider_url;
    u64 timestamp_ms; // embed.timestamp; 0 when absent
    GatewayMessageEmbedMedia image;
    GatewayMessageEmbedMedia thumbnail;
    GatewayMessageEmbedMedia video;
    u32 color;
    u16 field_begin;
    u16 field_count;
    GatewayMessageEmbedType type;
    bool has_color;
    bool has_title;
    bool has_author;
    u8 flags; // embed flags (low byte): CONTAINS_EXPLICIT_MEDIA = 16
};

// Ordinary authored rows (user message types plus thread starters,
// which render their referenced message); every other type is a compact
// system row whose text is generated from the type and its references.
constexpr bool message_type_is_system(u16 type) noexcept {
    return !(type == 0 || type == 19 || type == 20 || type == 21 || type == 23 || type == 26 || type == 41 ||
             type == 45 || type == 47 || type == 68);
}

// Attachment and embed safety flags (the flags fields of both objects).
inline constexpr u32 kGatewayAttachmentSpoilerFlag = 8;
inline constexpr u32 kGatewayMediaExplicitFlag = 16;

struct GatewayMessageSticker {
    u64 id;
    Utf8View name;
    u8 format_type;
};

// Component flags beyond disabled/accent: emoji animation, media spoilers,
// separator dividers and select options that are selected by default.
enum GatewayMessageComponentFlag : u8 {
    gateway_message_component_emoji_animated = 1u << 0,
    gateway_message_component_spoiler = 1u << 1,
    gateway_message_component_divider = 1u << 2,
    gateway_message_component_default = 1u << 3,
};
inline constexpr u8 kGatewayMessageComponentKnownFlags =
    gateway_message_component_emoji_animated | gateway_message_component_spoiler |
    gateway_message_component_divider | gateway_message_component_default;
// Wire component types plus two local child kinds.
// Select options and media gallery items are carried as pseudo-components
// parented to their select/gallery so the bounded tree transports them.
enum GatewayMessageComponentType : u16 {
    gateway_component_action_row = 1,
    gateway_component_button = 2,
    gateway_component_string_select = 3,
    gateway_component_user_select = 5,
    gateway_component_role_select = 6,
    gateway_component_mentionable_select = 7,
    gateway_component_channel_select = 8,
    gateway_component_section = 9,
    gateway_component_text_display = 10,
    gateway_component_thumbnail = 11,
    gateway_component_media_gallery = 12,
    gateway_component_file = 13,
    gateway_component_separator = 14,
    gateway_component_container = 17,
    gateway_component_select_option = 0x7F01,
    gateway_component_gallery_item = 0x7F02,
};
constexpr bool gateway_component_is_select(u16 type) noexcept {
    return type == gateway_component_string_select || (type >= gateway_component_user_select &&
                                                       type <= gateway_component_channel_select);
}

// Field reuse by type: selects keep their placeholder in content; options keep
// value in custom_id and description in content; thumbnails and gallery items
// keep the media URL in url and alt text in content; separators keep spacing in
// style; files keep the file name in label.
struct GatewayMessageComponent {
    Utf8View label;
    Utf8View content;
    Utf8View custom_id;
    Utf8View url;
    Utf8View emoji_name;
    u64 emoji_id;
    u32 id;
    u32 accent_color;
    u16 parent;
    u16 order;
    u16 type;
    u16 style;
    bool disabled;
    bool has_accent_color;
    u8 min_values;
    u8 max_values;
    u8 flags;
};

// Call metadata of a CALL (type 3) system message.
struct GatewayMessageCall {
    u64 ended_timestamp_ms;
    bool present;
    bool ended;
    bool participated; // the current user is in participants
};

struct GatewayMessageSnapshotScratch {
    Slice<GatewayMessageAttachment> attachments;
    Slice<GatewayMessageEmbed> embeds;
    Slice<GatewayMessageEmbedField> embed_fields;
    Slice<GatewayMessageSticker> stickers;
    Slice<GatewayMessageComponent> components;
    Slice<GatewayMessageNamedUser> mention_users;
};

struct GatewayMessageSnapshot {
    Utf8View content;
    u64 timestamp_ms;
    u64 edited_timestamp_ms;
    ConstSlice<GatewayMessageAttachment> attachments;
    ConstSlice<GatewayMessageEmbed> embeds;
    ConstSlice<GatewayMessageEmbedField> embed_fields;
    ConstSlice<GatewayMessageSticker> stickers;
    ConstSlice<GatewayMessageComponent> components;
    ConstSlice<GatewayMessageNamedUser> mention_users;
    u32 flags;
    u32 fields;
    u32 rich_truncations;
    u16 type;
    bool present;
};

inline constexpr u32 kGatewayMessagePollMaximumAnswers = 10;
inline constexpr u32 kGatewayMessagePollQuestionMaximumScalars = 300;
inline constexpr u32 kGatewayMessagePollQuestionMaximumBytes = 1200;
inline constexpr u32 kGatewayMessagePollAnswerMaximumScalars = 55;
inline constexpr u32 kGatewayMessagePollAnswerMaximumBytes = 220;
inline constexpr u32 kGatewayMessagePollEmojiMaximumBytes = 128;

enum GatewayMessagePollField : u32 {
    gateway_message_poll_field_question = 1u << 0,
    gateway_message_poll_field_answers = 1u << 1,
    gateway_message_poll_field_expiry = 1u << 2,
    gateway_message_poll_field_multiselect = 1u << 3,
    gateway_message_poll_field_layout = 1u << 4,
    gateway_message_poll_field_results = 1u << 5,
};
struct GatewayMessagePollAnswer {
    u32 answer_id;
    Utf8View text;
    u64 emoji_id;
    Utf8View emoji_name;
    bool emoji_animated;
};
struct GatewayMessagePollAnswerCount {
    u32 answer_id;
    u32 count;
    bool me_voted;
};
struct GatewayMessagePoll {
    Utf8View question;
    ConstSlice<GatewayMessagePollAnswer> answers;
    ConstSlice<GatewayMessagePollAnswerCount> answer_counts;
    u64 expiry_ms;
    u32 fields;
    u16 layout_type;
    bool allow_multiselect;
    bool expiry_null;
    bool results_known;
    bool is_finalized;
    bool present;
};
struct GatewayMessagePollScratch {
    Slice<GatewayMessagePollAnswer> answers;
    Slice<GatewayMessagePollAnswerCount> answer_counts;
};
struct GatewayMessagePollVoteEvent {
    u64 channel_id;
    u64 message_id;
    u64 user_id;
    u64 guild_id;
    u32 answer_id;
    bool add;
    bool has_guild;
    // One of several votes sharing a MESSAGE_POLL_VOTE_ADD_MANY sequence.
    bool batched;
};

struct GatewayMessageRichScratch {
    Slice<GatewayMessageReaction> reactions;
    Slice<GatewayMessageAttachment> attachments;
    Slice<GatewayMessageEmbed> embeds;
    Slice<GatewayMessageEmbedField> embed_fields;
    Slice<GatewayMessageSticker> stickers;
    Slice<GatewayMessageComponent> components;
    GatewayMessageSnapshotScratch snapshot;
    GatewayMessagePollScratch poll;
};

enum GatewayMessageField : u32 {
    gateway_message_field_id = 1u << 0,
    gateway_message_field_channel_id = 1u << 1,
    gateway_message_field_author_id = 1u << 2,
    gateway_message_field_content = 1u << 3,
    gateway_message_field_timestamp = 1u << 4,
    gateway_message_field_edited_timestamp = 1u << 5,
    gateway_message_field_flags = 1u << 6,
    gateway_message_field_mention_everyone = 1u << 7,
    gateway_message_field_mentions = 1u << 8,
    gateway_message_field_mention_roles = 1u << 9,
    gateway_message_field_message_reference = 1u << 10,
    gateway_message_field_referenced_message = 1u << 11,
    gateway_message_field_reactions = 1u << 12,
    gateway_message_field_attachments = 1u << 13,
    gateway_message_field_embeds = 1u << 14,
    gateway_message_field_sticker_items = 1u << 15,
    gateway_message_field_components = 1u << 16,
    gateway_message_field_nonce = 1u << 17,
    gateway_message_field_member = 1u << 18,
    gateway_message_field_interaction = 1u << 19,
    gateway_message_field_type = 1u << 20,
    gateway_message_field_author_policy = 1u << 21,
    gateway_message_field_activity_instance = 1u << 22,
    gateway_message_field_application_id = 1u << 23,
    gateway_message_field_changelog_id = 1u << 24,
    gateway_message_field_interaction_metadata = 1u << 25,
    gateway_message_field_pinned = 1u << 26,
    gateway_message_field_message_snapshots = 1u << 27,
    gateway_message_field_poll = 1u << 28,
    gateway_message_field_call = 1u << 29,
    gateway_message_field_activity = 1u << 30,
    gateway_message_field_thread = 1u << 31,
};

// The thread started from this message (a channel object); null clears it.
struct GatewayMessageThread {
    u64 id;
    u64 last_message_id;
    Utf8View name;
    u32 message_count;
    u32 member_count;
    bool archived;
    bool locked;
    bool present;
};

enum GatewayMessageForwardingFeature : u8 {
    gateway_message_forwarding_poll = 1u << 0,
    gateway_message_forwarding_call = 1u << 1,
    gateway_message_forwarding_activity = 1u << 2,
};

struct GatewayMessageEvent {
    u64 id;
    u64 channel_id;
    u64 author_id;
    u64 timestamp_ms;
    u64 edited_timestamp_ms;
    Utf8View content;
    u32 flags;
    u32 fields;
    ConstSlice<u64> mention_roles;
    ConstSlice<u64> member_roles;
    bool mention_everyone;
    bool mentions_current_user;
    Utf8View author_name;
    Utf8View author_avatar_hash;
    // The author's server nickname from member.nick; empty when the member
    // has none or the event carries no member.
    Utf8View member_nick;
    ConstSlice<GatewayMessageNamedUser> mention_users;
    GatewayMessageReference reference;
    GatewayMessageInteraction interaction;
    ConstSlice<GatewayMessageReaction> reactions;
    ConstSlice<GatewayMessageAttachment> attachments;
    ConstSlice<GatewayMessageEmbed> embeds;
    ConstSlice<GatewayMessageEmbedField> embed_fields;
    ConstSlice<GatewayMessageSticker> stickers;
    ConstSlice<GatewayMessageComponent> components;
    u32 rich_truncations;
    u32 transient_bytes;
    u64 nonce;
    u32 author_public_flags;
    u16 type;
    bool author_bot, activity_instance_present, application_present, changelog_present,
        interaction_metadata_present;
    bool pinned;
    u8 forwarding_features; // object presence only; body remains in the shared JSON scratch
    // member.roles did not fit the caller's scratch: member_roles is empty and
    // the author's roles are unknown.
    bool member_roles_partial;
    GatewayMessageSnapshot snapshot;
    GatewayMessagePoll poll;
    GatewayMessageThread thread;
    GatewayMessageCall call;
};

struct GatewayMessageDeleteEvent {
    u64 id;
    u64 channel_id;
};

struct GatewayMessageBulkDeleteEvent {
    u64 channel_id;
    u32 id_count;
};

bool gateway_message_poll_valid(const GatewayMessagePoll& poll) noexcept;

// Poll strings borrow caller-owned decoded text; no view borrows JSON input.
// Omitted message fields are untouched; top-level null sets present=false.
// A supplied poll object may be partial. Missing results do not mean zero votes.
// Explicit results:null is rejected because the API schema permits only objects.
GatewayEventDecodeResult gateway_decode_message_poll(JsonValue data, const JsonIndexedDocument& document,
                                                     u32 maximum_depth, Slice<char> text_output,
                                                     u32* text_used,
                                                     const GatewayMessagePollScratch& scratch,
                                                     GatewayMessagePoll* output,
                                                     bool require_complete = false) noexcept;
GatewayEventDecodeResult gateway_decode_message_poll_vote(JsonValue data, Slice<u32> structural_scratch,
                                                          u32 maximum_depth, bool add,
                                                          GatewayMessagePollVoteEvent* output) noexcept;

// Decodes the API's RFC 3339/ISO 8601 timestamp representation to Unix
// milliseconds. Fractional precision beyond milliseconds is validated and
// truncated, matching the model's storage precision.
bool parse_iso_timestamp(const JsonString& value, u64* timestamp_ms) noexcept;

// Content is unescaped into caller storage; the returned view is valid only
// while that storage is. The fields mask distinguishes an absent MESSAGE_UPDATE
// field from a present empty or null value. Snapshot core fields are partial
// and carry no message/channel/author identity.
//
// Overloads taking a JsonIndexedDocument decode a value of an already indexed document
// (a page of messages): nothing is scanned and the index stays intact.
GatewayEventDecodeResult gateway_decode_message_snapshot_core(const JsonIndexedDocument& document, JsonValue data,
                                                              u32 maximum_depth,
                                                              Slice<char> content_scratch,
                                                              Slice<GatewayMessageNamedUser> mention_users,
                                                              GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_extract_message_snapshot(const JsonIndexedDocument& document, JsonValue array,
                                                          u32 maximum_depth, JsonValue* body,
                                                          bool* present) noexcept;
GatewayEventDecodeResult gateway_decode_message_create(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_create(const JsonIndexedDocument& document, JsonValue data,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_create(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       Slice<u64> mention_role_scratch,
                                                       Slice<GatewayMessageNamedUser> mention_user_scratch,
                                                       u64 current_user_id,
                                                       GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_create(const JsonIndexedDocument& document, JsonValue data,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       Slice<u64> mention_role_scratch,
                                                       Slice<GatewayMessageNamedUser> mention_user_scratch,
                                                       u64 current_user_id,
                                                       GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_create(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> content_scratch,
    Slice<u64> mention_role_scratch, Slice<u64> member_role_scratch,
    Slice<GatewayMessageNamedUser> mention_user_scratch, const GatewayMessageRichScratch& rich_scratch,
    u64 current_user_id, GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_create(
    const JsonIndexedDocument& document, JsonValue data, u32 maximum_depth, Slice<char> content_scratch,
    Slice<u64> mention_role_scratch, Slice<u64> member_role_scratch,
    Slice<GatewayMessageNamedUser> mention_user_scratch, const GatewayMessageRichScratch& rich_scratch,
    u64 current_user_id, GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_update(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth, Slice<char> content_scratch,
                                                       GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_update(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> content_scratch,
    Slice<u64> mention_role_scratch, Slice<u64> member_role_scratch,
    Slice<GatewayMessageNamedUser> mention_user_scratch, const GatewayMessageRichScratch& rich_scratch,
    u64 current_user_id, GatewayMessageEvent* output) noexcept;

GatewayEventDecodeResult gateway_decode_message_rich_fields(JsonValue data, Slice<u32> structural_scratch,
                                                            u32 maximum_depth, Slice<char> text_output,
                                                            u32* text_used,
                                                            const GatewayMessageRichScratch& scratch,
                                                            GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_rich_fields(const JsonIndexedDocument& document, JsonValue data,
                                                            u32 maximum_depth, Slice<char> text_output,
                                                            u32* text_used,
                                                            const GatewayMessageRichScratch& scratch,
                                                            GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_delete(JsonValue data, Slice<u32> structural_scratch,
                                                       u32 maximum_depth,
                                                       GatewayMessageDeleteEvent* output) noexcept;
GatewayEventDecodeResult gateway_decode_message_bulk_delete(JsonValue data, Slice<u32> structural_scratch,
                                                            u32 maximum_depth, Slice<u64> ids,
                                                            GatewayMessageBulkDeleteEvent* output) noexcept;

} // namespace maicord
