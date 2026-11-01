#pragma once
#include "maicord/app/gateway/client_limits.hpp"
#include "maicord/model/chat/upload_allowance.hpp"
#include "maicord/app/gateway/read_state.hpp"
#include "maicord/protocol/gateway/events.hpp"
#include "maicord/protocol/gateway/message_data.hpp"

namespace maicord {

struct GatewayMessageReactionEvent;

struct UserAppearance;

struct RelationshipMutation;

enum class GatewaySessionError : u8;
enum class WebSocketError : u8;
enum class WebSocketHandshakeError : u8;
enum class ZstdError : u8;
enum class EtfError : u8;

enum class VoiceSocketPhase : u8;
enum class VoiceConnectionError : u8;
enum class VoiceUdpPhase : u8;
enum class VoiceUdpError : u8;
enum class VoiceSourceKind : u8;
enum class VoiceVideoCodec : u8;
struct GatewayStreamKey;
struct GatewayClientStreamSnapshot;

enum class GatewayClientPhase : u8 {
    empty,
    resolving,
    connecting,
    tls_handshake,
    websocket_opening,
    awaiting_hello,
    hello,
    identifying,
    resuming,
    ready,
    reconnect_wait,
    stopping,
    stopped,
    failed,
};

enum class GatewayClientIdentity : u8 {
    empty,
    normal_user,
};

enum class GatewayClientError : u8 {
    none,
    invalid_input,
    memory,
    event,
    worker,
    random,
    resolve,
    connect,
    tls,
    websocket_opening,
    send,
    receive,
    session,
    timeout,
};

// Non-secret worker boundary kept only when a GatewayClientError::session
// failure is reported. This distinguishes transport/session decoding from
// local READY/dispatch routing and queue invariants without keeping payloads.
enum class GatewayClientFailureStage : u8 {
    none,
    pong_queue,
    pong_send,
    hello_heartbeat,
    hello_action,
    heartbeat_request,
    ready_identity,
    ready_resume_capture,
    ready_route,
    ready_publish,
    ready_guild_route,
    dispatch_route,
    close_stop,
    close_random,
    close_reconnect,
    reconnect_request,
    invalid_session_random,
    invalid_session_reconnect,
    session_event,
    tls_decrypt,
    session_push,
    application_output,
    ready_private_route,
    ready_user_route,
};

enum class GatewayClientEventKind : u8 {
    websocket_opened,
    hello,
    identifying,
    resuming,
    ready,
    resumed,
    dispatch,
    heartbeat_sent,
    heartbeat_acknowledged,
    reconnecting,
    closed,
    failed,
    stopped,
};

enum class GatewayClientDispatchKind : u8 {
    none,
    unknown,
    guild_create,
    guild_update,
    guild_delete,
    channel_create,
    channel_update,
    channel_delete,
    guild_role_create,
    guild_role_update,
    guild_role_delete,
    guild_member_update,
    guild_emojis_update,
    guild_member_list_update,
    thread_member_list_update,
    thread_members_update,
    typing_start,
    message_create,
    message_update,
    message_delete,
    message_bulk_delete,
    message_reaction_add,
    message_reaction_remove,
    message_reaction_remove_all,
    message_reaction_remove_emoji,
    // The wire event was message-related but could not be represented by the
    // decoder. The consumer must REST-catch-up without terminating the
    // Gateway session.
    message_catchup_required,
    presence_update,
    user_snapshot,
    channel_recipient_add,
    channel_recipient_remove,
    private_state_incomplete,
    ready_state_complete,
    supplemental_state_complete,
    command_autocomplete,
    account_required_action,
    relationship_snapshot_begin,
    relationship_snapshot_item,
    relationship_snapshot_complete,
    relationship_add,
    relationship_update,
    relationship_remove,
    relationship_state_incomplete,
    read_state_snapshot,
    message_ack,
    notification_snapshot_begin,
    notification_settings_entry,
    notification_snapshot_complete,
    notification_global,
    notification_thread,
    notification_incomplete,
    guild_members_chunk,
    guild_member_profile_update,
    channel_pins_update,
    thread_self_update,
    thread_list_sync,
    message_poll_vote_add,
    message_poll_vote_remove,
    // Session-free voice states; payload is GatewayClientVoiceStateRecord[record_count].
    voice_state_snapshot,
    // member_list.guild_id plus one GatewayClientUserMutation record.
    guild_member_remove,
    guild_ban_add,
    guild_ban_remove,
    // Verbatim PreloadedUserSettings field 14 (GuildFolders); empty when absent.
    guild_folders,
    // Stored PreloadedUserSettings groups (user_settings.hpp).
    user_settings,
    // CALL_CREATE/CALL_UPDATE: state.id is the DM or group channel and
    // state.record_count is 1 while the current user is being rung.
    call_update,
    // CALL_DELETE: state.id is the channel whose call ended.
    call_delete,
    // GUILD_STICKERS_UPDATE, and each READY guild or GUILD_CREATE with stickers:
    // member_list.guild_id and the verbatim sticker array JSON (empty when it
    // did not fit; the list is then refetched).
    guild_stickers_update,
    // Stage instances: voice_states header (guild_id, record_count, replace)
    // and GatewayClientStageInstanceRecord[record_count]. A record with a zero
    // id ends the stage of its channel (STAGE_INSTANCE_DELETE).
    stage_instance_snapshot,
    // Interaction lifecycle for requests this session sent: the payload is
    // the verbatim JSON object (protocol/messages/interaction.hpp decodes it).
    interaction_create,
    interaction_success,
    interaction_failure,
    interaction_modal_create,
    // Scheduled events, verbatim JSON. The snapshot carries a guild's
    // guild_scheduled_events array (READY guilds, GUILD_CREATE) with the guild
    // in voice_states.guild_id and replaces that guild's events; the others
    // carry the dispatch object (protocol/guild/community.hpp decodes both).
    guild_scheduled_event_snapshot,
    guild_scheduled_event_create,
    guild_scheduled_event_update,
    guild_scheduled_event_delete,
    guild_scheduled_event_user_add,
    guild_scheduled_event_user_remove,
    // USER_NOTE_UPDATE ({"id","note"}), verbatim JSON.
    user_note_update,
    // Soundboard, verbatim JSON (soundboard.hpp decodes them):
    // VOICE_CHANNEL_EFFECT_SEND, GUILD_SOUNDBOARD_SOUND_CREATE/UPDATE/DELETE,
    // GUILD_SOUNDBOARD_SOUNDS_UPDATE and SOUNDBOARD_SOUNDS (op 31 answers).
    voice_channel_effect_send,
    guild_soundboard_sound_create,
    guild_soundboard_sound_update,
    guild_soundboard_sound_delete,
    guild_soundboard_sounds_update,
    soundboard_sounds,
    // GUILD_BULK_BAN_UPDATE, verbatim JSON (protocol/guild/moderation.hpp decodes it).
    guild_bulk_ban_update,
    // state.guild_id and state.record_count, followed by ChannelCursor records.
    // Latest activity only: never a read cursor, message body or mention count.
    channel_cursor_update,
    // GUILD_APPLICATION_COMMAND_INDEX_UPDATE: state.guild_id only; no payload.
    guild_command_index_update,
    // Voice channel extras, verbatim JSON (protocol/voice/channel_info.hpp decodes them):
    // CHANNEL_INFO (opcode 43 answers), VOICE_CHANNEL_START_TIME_UPDATE and
    // VOICE_CHANNEL_STATUS_UPDATE.
    channel_info,
    voice_channel_start_time_update,
    voice_channel_status_update,
};

enum class GatewayClientDispatchRouteError : u8 {
    none,
    invalid_input,
    event_name_capacity,
    payload_capacity,
    decode,
};

enum class GatewayClientPopError : u8 {
    none,
    invalid_input,
    empty,
    output_too_small,
    corrupt,
};

struct GatewayClientChannelCursor {
    u64 channel_id;
    u64 last_message_id; // Zero is a nullable cursor with no forward activity.
};
static_assert(sizeof(GatewayClientChannelCursor) == 16);
inline constexpr u32 kGatewayChannelCursorMaximumEntries =
    kGatewayClientDispatchPayloadCapacity / sizeof(GatewayClientChannelCursor);
// A whole guild snapshot at the server's own limits (500 channels plus active
// threads with 160-byte headers, names and 32 bytes per permission overwrite,
// 250 roles, the emoji list) can exceed the ordinary capacity. Only such a
// record is routed again into a reserved buffer committed on demand up to
// this bound and handed to the UI outside the ring; nothing stays committed
// once it is applied. A guild that still does not fit is listed as unavailable.
inline constexpr u32 kGatewayClientGuildRecordCapacity = 1024 * 1024;
static_assert(kGatewayClientGuildRecordCapacity > kGatewayClientDispatchPayloadCapacity);

enum GatewayClientStateField : u16 {
    gateway_client_state_field_id = 1u << 0,
    gateway_client_state_field_guild_id = 1u << 1,
    gateway_client_state_field_parent_id = 1u << 2,
    gateway_client_state_field_name = 1u << 3,
    gateway_client_state_field_position = 1u << 4,
    gateway_client_state_field_flags = 1u << 5,
    gateway_client_state_field_type = 1u << 6,
    gateway_client_state_field_unavailable = 1u << 7,
    gateway_client_state_field_owner_id = 1u << 8,
    gateway_client_state_field_permissions = 1u << 9,
    gateway_client_state_field_permission_overwrites = 1u << 10,
    gateway_client_state_field_last_message_id = 1u << 11,
    gateway_client_state_field_icon = 1u << 12,
    gateway_client_state_field_color = 1u << 13,
    gateway_client_state_field_hoist = 1u << 14,
    gateway_client_state_field_managed = 1u << 15,
};

enum GatewayClientMessageMentionFlag : u8 {
    gateway_client_message_mention_everyone = 1u << 0,
    gateway_client_message_mention_current_user = 1u << 1,
    gateway_client_message_author_bot = 1u << 2,
    gateway_client_message_activity_instance = 1u << 3,
    gateway_client_message_application_present = 1u << 4,
    gateway_client_message_changelog_present = 1u << 5,
    gateway_client_message_interaction_metadata_present = 1u << 6,
    gateway_client_message_pinned = 1u << 7,
};

struct GatewayClientMessageMutation {
    u64 id;
    u64 channel_id;
    u64 author_id;
    u64 timestamp_ms;
    u64 edited_timestamp_ms;
    u32 flags;
    u32 fields;
    // Bulk deletion uses it as an ID count; create/update use it for object-presence policy.
    // The dispatch kind discriminates these values, keeping the mailbox compact.
    union {
        u32 id_count;
        u32 forwarding_features;
    };
    u32 content_bytes;
    u16 mention_role_count;
    u16 member_role_count;
    u8 mention_flags;
    u8 author_avatar_hash_bytes;
    u16 type; // occupies two bytes of alignment padding
    u32 transient_bytes;
    u16 mention_user_count;
    u8 author_name_bytes;
    // The author's server nickname, after the avatar hash in the payload text.
    u8 member_nick_bytes;
    u32 rich_payload_bytes;
    u32 author_public_flags; // occupies four bytes of nonce alignment padding
    u64 nonce;
};

struct GatewayClientMessageReactionEvent {
    u64 message_id;
    u64 channel_id;
    u64 user_id;
    u64 emoji_id;
    u16 name_bytes;
    u8 type;
    u8 flags;
};

struct GatewayClientTypingStartEvent {
    u64 channel_id;
    u64 guild_id;
    u64 user_id;
    u64 timestamp_seconds;
};

// Pointer-free voice-state rows. A snapshot with replace set first removes every
// state of guild_id (zero: private calls); continuation chunks only apply rows.
struct GatewayClientVoiceStateRecord {
    u64 guild_id;
    u64 channel_id; // Zero removes the user's state.
    u64 user_id;
    u32 flags;      // GatewayVoiceFlag
    u32 request_to_speak_s; // Unix seconds while gateway_voice_request_to_speak is set.
};
static_assert(sizeof(GatewayClientVoiceStateRecord) == 32);
inline constexpr u32 kGatewayClientStageTopicBytes = 480; // 120 code points.
struct GatewayClientStageInstanceRecord {
    u64 id; // Zero: the channel's stage ended.
    u64 guild_id;
    u64 channel_id;
    u16 topic_bytes;
    u8 privacy_level;
    u8 reserved[5];
    char topic[kGatewayClientStageTopicBytes];
};
static_assert(sizeof(GatewayClientStageInstanceRecord) == 512);
struct GatewayClientVoiceStateSnapshot {
    u64 guild_id;
    u32 record_count;
    bool replace;
};

struct GatewayClientNamedUserMutation {
    u64 id;
    u32 name_offset;
    u16 name_bytes;
    u16 reserved;
};

struct GatewayClientUserMutation {
    u64 id;
    u16 name_bytes;
    u8 avatar_bytes;
    // bit 0: display name known; bit 1: avatar known (zero bytes clears it);
    // bit 2: premium type known; bit 3: two-factor state known; bit 4: two-factor enabled;
    // bits 5/6/7: nsfw_allowed present / nonnull / allowed.
    u8 flags;
    u32 premium_type;
    u16 username_bytes;
    u16 appearance_bytes;
    u8 age_verification_status; // AccountAgeVerification; zero is unknown.
    // bit 0: age_verification_status present (null clears);
    // bits 1/2: linked_users present / active parent found.
    u8 content_fields;
    u16 reserved;
};
static_assert(sizeof(GatewayClientUserMutation) == 24);
bool gateway_client_user_record(ConstSlice<u8> payload, u32* cursor, GatewayClientUserMutation* user,
                                Utf8View* name, Utf8View* avatar, Utf8View* username = nullptr,
                                ConstSlice<u8>* appearance = nullptr) noexcept;
// Scans every linked row without retaining it. Invalid arrays leave output unchanged.
bool gateway_client_linked_parent(const JsonIndexedDocument& document, JsonValue value,
    u32 maximum_depth, bool* parent_managed) noexcept;

// Every rich-message string remains in the leading transient-text area of the
// mailbox payload. These ranges make the trailing descriptor block entirely
// pointer-free and safe to retain after the Gateway JSON buffer is reused.
struct GatewayClientMessageTextRange {
    u32 offset;
    u16 bytes;
    u16 reserved;
};

struct GatewayClientMessageRichHeader {
    u64 reference_message_id;
    u64 reference_channel_id;
    u64 reference_guild_id;
    u64 reference_author_id;
    GatewayClientMessageTextRange reference_author_name;
    GatewayClientMessageTextRange reference_author_avatar_hash;
    GatewayClientMessageTextRange reference_content;
    u64 interaction_id;
    u64 interaction_user_id;
    GatewayClientMessageTextRange interaction_user_name;
    GatewayClientMessageTextRange interaction_user_avatar_hash;
    GatewayClientMessageTextRange interaction_command_name;
    u32 truncations;
    u16 reaction_count;
    u16 attachment_count;
    u16 embed_count;
    u16 embed_field_count;
    u16 sticker_count;
    u16 component_count;
    u8 reference_state;
    u8 reference_type;
    u8 interaction_type;
    u8 reference_content_flags; // GatewayMessageReferenceContent
    u8 reference_message_type;
    u8 reference_quote_flags; // GatewayMessageReferenceQuote
    u8 reserved[2];
};

// Present only for messages whose fields contain gateway_message_field_poll.
struct GatewayClientMessagePollHeader {
    u64 expiry_ms;
    GatewayClientMessageTextRange question;
    u32 fields;
    u16 layout_type;
    u8 answer_count;
    u8 count_count;
    u8 flags; // present, expiry-null, multiselect, results-known, finalized
    u8 reserved[7];
};
struct GatewayClientMessagePollAnswerMutation {
    u64 emoji_id;
    GatewayClientMessageTextRange text;
    GatewayClientMessageTextRange emoji_name;
    u32 answer_id;
    u8 flags;
    u8 reserved[3];
};
struct GatewayClientMessagePollCountMutation {
    u32 answer_id;
    u32 count;
    u8 flags;
    u8 reserved[3];
};

struct GatewayClientMessageSnapshotHeader {
    GatewayClientMessageTextRange content;
    u64 timestamp_ms;
    u64 edited_timestamp_ms;
    u32 flags;
    u32 fields;
    u32 rich_payload_bytes;
    u16 type;
    u16 mention_user_count;
    u8 present;
    u8 reserved[7];
};
static_assert(sizeof(GatewayClientMessageSnapshotHeader) == 48);

struct GatewayClientMessageReactionMutation {
    u64 emoji_id;
    GatewayClientMessageTextRange name;
    u32 count;
    u32 normal_count;
    u32 burst_count;
    u8 flags;
    u8 reserved[3];
};

struct GatewayClientMessageAttachmentMutation {
    u64 id;
    u64 size;
    GatewayClientMessageTextRange filename;
    GatewayClientMessageTextRange title;
    GatewayClientMessageTextRange description;
    GatewayClientMessageTextRange content_type;
    GatewayClientMessageTextRange url;
    u32 width;
    u32 height;
    u32 flags;
    u8 state;
    u8 reserved[3];
    GatewayClientMessageTextRange waveform;
    u32 duration_ms;
    u32 audio_reserved;
};

struct GatewayClientMessageEmbedMediaMutation {
    GatewayClientMessageTextRange url;
    GatewayClientMessageTextRange proxy_url;
    GatewayClientMessageTextRange content_type;
    GatewayClientMessageTextRange description;
    u32 width;
    u32 height;
    u32 flags;
    u8 state;
    u8 reserved[3];
};

struct GatewayClientMessageEmbedMutation {
    GatewayClientMessageTextRange title;
    GatewayClientMessageTextRange description;
    GatewayClientMessageTextRange url;
    GatewayClientMessageTextRange author_name;
    GatewayClientMessageTextRange footer_text;
    GatewayClientMessageTextRange author_url;
    GatewayClientMessageTextRange author_icon;
    GatewayClientMessageTextRange footer_icon;
    GatewayClientMessageTextRange provider_name;
    GatewayClientMessageTextRange provider_url;
    GatewayClientMessageEmbedMediaMutation image;
    GatewayClientMessageEmbedMediaMutation thumbnail;
    GatewayClientMessageEmbedMediaMutation video;
    u32 color;
    u32 timestamp_low;
    u32 timestamp_high;
    u16 field_begin;
    u16 field_count;
    u8 type;
    u8 flags;
    u8 reserved[2];
};

struct GatewayClientMessageEmbedFieldMutation {
    GatewayClientMessageTextRange name;
    GatewayClientMessageTextRange value;
    u16 embed_index;
    u8 flags;
    u8 reserved;
};

struct GatewayClientMessageStickerMutation {
    u64 id;
    GatewayClientMessageTextRange name;
    u8 format_type;
    u8 reserved[7];
};

// flags: 1 disabled, 2 accent colour; component_flags carries
// GatewayMessageComponentFlag bits.
struct GatewayClientMessageComponentMutation {
    GatewayClientMessageTextRange label;
    GatewayClientMessageTextRange content;
    GatewayClientMessageTextRange custom_id;
    GatewayClientMessageTextRange url;
    GatewayClientMessageTextRange emoji_name;
    u64 emoji_id;
    u32 id;
    u32 accent_color;
    u16 parent;
    u16 order;
    u16 type;
    u16 style;
    u8 flags;
    u8 min_values;
    u8 max_values;
    u8 component_flags;
    u32 reserved;
};

// Present after the component descriptors when fields contain
// gateway_message_field_call. flags: 1 present, 2 ended, 4 participated.
struct GatewayClientMessageCallMutation {
    u64 ended_timestamp_ms;
    u8 flags;
    u8 reserved[7];
};

// Pointer-free state descriptor used both in the event union and as the
// fixed header of every channel record packed behind GUILD_CREATE. Names
// immediately follow their owning descriptor in UTF-8 form.
inline constexpr u8 kGatewayStateContentGuildLevelMarker = 1u << 7;
struct GatewayClientStateMutation {
    u64 id;
    u64 guild_id;
    u64 parent_id;
    u64 owner_id;
    u64 permissions;
    u64 last_message_id;
    u64 member_list_id;
    u32 position;
    u32 flags;
    union {
        u32 color;
        // Thread channel records: the current member's join time in Unix
        // seconds; zero when unknown.
        u32 thread_join_seconds;
    };
    u32 record_count;
    u32 role_count;
    u32 member_role_count;
    u32 overwrite_count;
    u32 emoji_count;
    u32 emoji_role_count;
    u16 name_bytes;
    u16 fields;
    u8 type;
    bool unavailable;
    bool hoist;
    bool managed;
    u8 icon_bytes;
    bool emojis_known;
    bool member_list_id_known;
    u16 recipient_count;
    bool recipients_known;
    bool private_nicknames_known;
    UploadPolicy upload_policy;
    bool upload_policy_known;
    u8 upload_features;
    bool upload_features_known;
    u8 thread_archived : 1;
    u8 thread_archived_known : 1;
    u8 thread_locked : 1;
    u8 thread_locked_known : 1;
    u8 thread_self_joined : 1;
    u8 thread_self_joined_known : 1;
    u8 read_state_partial : 1;
    u8 content_policy_known : 1;
    // Channel: nsfw bit; Guild: marker | documented nsfw_level (0 through 3).
    u8 content_policy;
    // Guild create/update: 0 when the object carried no "banner" key, else 1
    // plus the banner hash bytes that follow the icon (1: a null banner).
    // Zero on channel records and every other event.
    u8 guild_banner;
    union {
        // Channel: slowmode seconds plus one (0 through 21601); zero means absent.
        u16 rate_limit_per_user;
        // Guild create/update: bytes of the GatewayGuildDetails record that
        // follows the banner; zero when the record has none.
        u16 guild_details_bytes;
    };
    // Guild settings (GUILD_CREATE/UPDATE) and the current member's timeout
    // (GUILD_CREATE members, own GUILD_MEMBER_UPDATE); see GuildSettings.
    u64 system_channel_id;
    u64 afk_channel_id;
    u32 afk_timeout;
    u32 self_communication_disabled_until;
    u16 guild_settings_known;
    u8 mfa_level;
    u8 verification_level;
    u8 default_message_notifications;
    u8 explicit_content_filter;
    union {
        // Role objects: GatewayClientRoleFlag bits.
        u8 role_flags;
        // guild_create / own guild_member_update: presence of role authority.
        u8 member_role_flags;
        // Channel dispatches: GatewayClientChannelEventFlag bits.
        u8 channel_event_flags;
        // READY only: AccountContentFlag bits.
        u8 account_content_flags;
    };
    union {
        // The current member's GuildSelfMember bits (with guild_settings_self_member).
        u8 self_member;
        // Channel: default_auto_archive_duration as a channel_archive_code (1
        // hour, 1 day, 3 days, 1 week: 1..4); zero when absent or unsupported.
        u8 default_archive_code;
    };
    // Role objects: enhanced colours (colors.secondary_color/tertiary_color),
    // RGB | kRoleColorPresent, or zero when absent.
    u32 secondary_color;
    u32 tertiary_color;
};
// Private channel payloads may end with a trailer after the nickname records:
// one GatewayClientPrivateFlag byte, then the warning id (u64 LE) when a
// safety warning type is set in bits 5..7. It is present only when the
// channel carried message request or safety warning fields.
enum GatewayClientPrivateFlag : u8 {
    gateway_client_private_request_known = 1u << 0,
    gateway_client_private_request_pending = 1u << 1,
    gateway_client_private_request_spam = 1u << 2,
    gateway_client_private_safety_known = 1u << 3,
};
inline constexpr u32 kGatewayClientPrivateSafetyShift = 5;
constexpr u8 gateway_client_private_safety_type(u8 flags) noexcept {
    return static_cast<u8>(flags >> kGatewayClientPrivateSafetyShift);
}

enum GatewayClientMemberRoleFlag : u8 {
    gateway_client_member_roles_known = 1u << 0,
};
enum GatewayClientRoleFlag : u8 {
    gateway_client_role_hoist = 1u << 0,
    gateway_client_role_managed = 1u << 1,
    gateway_client_role_mentionable = 1u << 2,
    gateway_client_role_has_icon = 1u << 3,
    gateway_client_role_in_prompt = 1u << 4,
    // tags.guild_connections is present: a linked role.
    gateway_client_role_linked = 1u << 5,
};
enum GatewayClientChannelEventFlag : u8 {
    // THREAD_CREATE with newly_created=true, never an old membership sync.
    gateway_client_channel_new_thread = 1u << 0,
};
static_assert(sizeof(GatewayClientMessageMutation) == 88);

struct GatewayClientRoleMutation {
    u64 id;
    u64 permissions;
    u32 position;
    u32 color;
    // Enhanced role colours: RGB | kRoleColorPresent, or zero when absent.
    u32 secondary_color;
    u32 tertiary_color;
    u16 name_bytes;
    u8 flags;
    u8 reserved[5];
};

struct GatewayClientPermissionOverwriteMutation {
    u64 id;
    u64 allow;
    u64 deny;
    u8 type;
    u8 reserved[7];
};

enum GatewayClientEmojiFlag : u8 {
    gateway_client_emoji_animated = 1u << 0,
    gateway_client_emoji_available = 1u << 1,
    gateway_client_emoji_managed = 1u << 2,
    gateway_client_emoji_require_colons = 1u << 3,
};

// Pointer-free descriptor packed after the owning guild's channel records.
// Restricted role IDs follow the descriptor in little-endian form, then the
// decoded UTF-8 name. GUILD_EMOJIS_UPDATE contains only these records.
struct GatewayClientEmojiMutation {
    u64 id;
    u16 name_bytes;
    u16 role_count;
    u8 flags;
    u8 reserved[3];
};

enum class GatewayClientMemberListOperation : u8 {
    sync,
    update,
    insert,
    remove,
    invalidate,
    group_summary,
};

enum class GatewayClientMemberListStatus : u8 {
    unknown,
    online,
    idle,
    dnd,
    offline,
};

struct GatewayClientPresenceUpdate {
    u64 guild_id;
    u64 user_id;
    GatewayClientMemberListStatus status;
    // client_status, two bits per platform (protocol/client_status.hpp).
    u8 client_status;
};

// Decodes a PRESENCE_UPDATE value of an indexed document (no scan); with appearance also its activities.
GatewayEventDecodeResult gateway_client_decode_presence(const JsonIndexedDocument& document, JsonValue data,
                                                        u32 maximum_depth,
                                                        GatewayClientPresenceUpdate* output,
                                                        UserAppearance* appearance = nullptr) noexcept;

enum class GatewayClientMemberListItemKind : u8 {
    member,
    group,
    summary
};
// Pointer-free member-list record. Records are packed sequentially, followed
// by role IDs, nickname bytes, then display-name bytes. SYNC is represented by
// an INVALIDATE range followed by its indexed member/group records, so the UI
// consumer can replace a range without keeping the Gateway JSON document.
// Group records use user_id for the group ID and range_end for its count.
// Root group summaries follow all positional operations; their indices and
// counts determine logical extent independently of guild member_count.
struct GatewayClientMemberListMutation {
    u64 user_id;
    u32 index;
    u32 range_end;
    u16 role_count;
    u16 nickname_bytes;
    u16 display_name_bytes;
    u16 avatar_hash_bytes;
    u16 username_bytes;
    u16 user_appearance_bytes;
    u16 member_appearance_bytes;
    GatewayClientMemberListOperation operation;
    GatewayClientMemberListStatus status;
    GatewayClientMemberListItemKind kind;
    // Thread packets use GatewayClientThreadHydrationFlag. Ordinary group
    // headers use GatewayClientMemberListGroupFlag and ordinary members use
    // GatewayClientMemberListHydrationFlag. Zero means full-role semantics.
    u8 hydration_flags;
    // The presence's client_status (protocol/client_status.hpp); meaningful
    // only when status is known.
    u8 client_status;
    // Unix seconds when gateway_member_timeout_known; zero clears a timeout.
    u32 communication_disabled_until;
};
static_assert(sizeof(GatewayClientMemberListMutation) == 40);

enum GatewayClientMemberListGroupFlag : u8 {
    gateway_member_group_count_omitted = 1u << 7,
};

enum GatewayClientMemberListHydrationFlag : u8 {
    gateway_member_quarantine_known = 1u << 0,
    gateway_member_quarantined = 1u << 1,
    // The member appearance ends with the presence's activities trailer
    // (model/user/activity.hpp), or the presence listed none.
    gateway_member_activities_known = 1u << 2,
    gateway_member_timeout_known = 1u << 3,
    gateway_member_roles_omitted = 1u << 6,
    gateway_member_nickname_omitted = 1u << 5,
    gateway_member_avatar_omitted = 1u << 4,
};

enum GatewayClientThreadHydrationFlag : u8 {
    gateway_thread_member_known = 1u << 0,
    gateway_thread_roles_known = 1u << 1,
    gateway_thread_nickname_known = 1u << 2,
    gateway_thread_username_known = 1u << 3,
    gateway_thread_avatar_known = 1u << 4,
    gateway_thread_presence_known = 1u << 5,
};
struct GatewayClientThreadMembersUpdate {
    u64 guild_id;
    u64 thread_id;
    u32 record_count;
    bool snapshot;
    bool incomplete;
    u8 authority_flags;
    u8 authority_reserved;
    u64 authority_self_id;
    // The current member's join time in Unix seconds when it joined; zero when unknown.
    u32 authority_join_seconds;
};
enum GatewayClientThreadAuthorityFlag : u8 {
    gateway_thread_self_known = 1,
    gateway_thread_self_joined = 2,
};
struct GatewayClientThreadSelfUpdate {
    u64 guild_id, thread_id, self_user_id;
    u8 authority_flags;
    u8 reserved[3];
    u32 join_seconds;
};
enum class GatewayClientThreadSyncScope : u8 {
    guild,
    parents
};
// THREAD_LIST_SYNC payload: parent ids, thread records, then one entry per
// joined thread: its id (u64 LE) and join time in seconds (u32 LE).
inline constexpr u32 kGatewayThreadSyncMemberBytes = 12;
// Discord omits `members` (a subscription's sync); listed threads then keep
// their known membership instead of reading as left.
inline constexpr u8 gateway_client_thread_sync_members_known = 1;
struct GatewayClientThreadListSync {
    u64 guild_id, self_user_id;
    u32 parent_count, thread_count, member_count;
    GatewayClientThreadSyncScope scope;
    u8 flags;
    u8 reserved[2];
};

struct GatewayClientMemberListUpdate {
    u64 guild_id;
    u64 list_id;
    u32 member_count;
    u32 online_count;
    u32 record_count;
    u32 member_record_count;
    u32 logical_count;
    u32 group_count;
    bool members_known;
    bool online_known;
    bool groups_known;
    // guild_member_profile_update only: the record came from GUILD_MEMBER_ADD.
    bool member_added;
};

enum GatewayClientNotificationFlag : u32 {
    gateway_notification_partial = 1,
    gateway_notification_incomplete = 2,
    gateway_notification_version_known = 4,
    gateway_notification_account_flags_known = 8,
    gateway_notification_guild_default_known = 16,
    gateway_notification_guild_default_mentions = 32,
    gateway_notification_thread_remove = 64,
    gateway_notification_restricted_known = 128,
    gateway_notification_restricted = 256,
    gateway_notification_channel_metadata = 512,
    gateway_notification_channel_age_baseline_complete = 1024,
};
// No sidecar header fit beside a valid ordinary body. Consumers invalidate
// policy metadata for the copied event's scope; ordinary apply remains valid.
inline constexpr u32 kGatewayNotificationMetadataIncomplete = ~u32{0};
struct GatewayClientNotificationProjection {
    u64 guild_id, channel_id, version;
    u32 override_count, flags, account_flags, reserved;
};
// Fixed sidecar header followed by thread records. Existing dispatch consumers
// strip notification_payload_bytes from the tail before their normal apply.
struct GatewayClientNotificationSidecar {
    u64 guild_id, channel_id;
    u32 thread_count, flags;
    u32 known_gates, gates;
};

struct GatewayClientEvent {
    u64 timestamp_ms;
    u64 sequence;
    u32 heartbeat_interval_ms;
    u32 native_error;
    u32 dispatch_payload_bytes;
    union {
        GatewayClientMessageMutation message;
        GatewayClientMessageReactionEvent reaction;
        GatewayMessagePollVoteEvent poll_vote;
        GatewayClientTypingStartEvent typing;
        GatewayClientVoiceStateSnapshot voice_states;
        GatewayClientStateMutation state;
        GatewayClientMemberListUpdate member_list;
        GatewayClientThreadMembersUpdate thread_members;
        GatewayClientThreadSelfUpdate thread_self;
        GatewayClientThreadListSync thread_sync;
        GatewayClientPresenceUpdate presence;
        GatewayClientReadStateEvent read_state;
        char event_name[kGatewayClientEventNameCapacity];
        AccountRequirement required_action;
        GatewayClientNotificationProjection notification;
    };
    u16 close_code;
    GatewayClientFailureStage failure_stage;
    GatewaySessionError session_error;
    WebSocketHandshakeError handshake_error;
    WebSocketError websocket_error;
    GatewayDecodeError gateway_decode_error;
    GatewayEventDecodeError event_decode_error;
    ZstdError compression_error;
    EtfError etf_error;
    JsonScanError json_scan_error;
    JsonReadError json_read_error;
    u32 route_error_offset;
    union {
        u32 route_detail;
        u32 notification_payload_bytes;
    };
    u8 event_name_bytes;
    GatewayClientEventKind kind;
    GatewayClientError error;
    GatewayClientDispatchKind dispatch_kind;
    bool has_sequence;
};

struct GatewayClientPopResult {
    u32 payload_bytes;
    GatewayClientPopError error;

    constexpr bool ok() const noexcept {
        return error == GatewayClientPopError::none;
    }
};

struct GatewayClientResult {
    u64 hello_elapsed_us;
    u32 native_error;
    GatewayClientError error;
    GatewayClientPhase phase;
    u16 close_code;

    constexpr bool ok() const noexcept {
        return error == GatewayClientError::none;
    }
};

} // namespace maicord
