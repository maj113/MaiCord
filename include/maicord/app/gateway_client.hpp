#pragma once
#include "maicord/core/memory.hpp"
#include "maicord/app/video_frame_sink.hpp"
#include "maicord/model/upload_allowance.hpp"
#include "maicord/app/gateway_state_recovery.hpp"
#include "maicord/app/gateway_voice_control.hpp"
#include "maicord/app/voice_audio_settings.hpp"
#include "maicord/app/gateway_read_state.hpp"

#include "maicord/app/account_profile.hpp"
#include "maicord/platform/network_worker.hpp"
#include "maicord/protocol/gateway_commands.hpp"
#include "maicord/app/gateway_user_configuration.hpp"
#include "maicord/protocol/gateway_endpoint.hpp"
#include "maicord/protocol/gateway_runtime.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/gateway_typing_events.hpp"
#include "maicord/model/message_notifications.hpp"
#include "maicord/protocol/self_status_settings.hpp"
#include "maicord/protocol/gateway_presence.hpp"

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
    // Stored PreloadedUserSettings groups (gateway_user_settings.hpp).
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
    // the verbatim JSON object (message_interaction.hpp decodes it).
    interaction_create,
    interaction_success,
    interaction_failure,
    interaction_modal_create,
    // Scheduled events, verbatim JSON. The snapshot carries a guild's
    // guild_scheduled_events array (READY guilds, GUILD_CREATE) with the guild
    // in voice_states.guild_id and replaces that guild's events; the others
    // carry the dispatch object (guild_community.hpp decodes both).
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
    // GUILD_BULK_BAN_UPDATE, verbatim JSON (guild_moderation.hpp decodes it).
    guild_bulk_ban_update,
    // state.guild_id and state.record_count, followed by ChannelCursor records.
    // Latest activity only: never a read cursor, message body or mention count.
    channel_cursor_update,
    // GUILD_APPLICATION_COMMAND_INDEX_UPDATE: state.guild_id only; no payload.
    guild_command_index_update,
    // Voice channel extras, verbatim JSON (voice_channel_info.hpp decodes them):
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

inline constexpr u32 kGatewayClientEventNameCapacity = 64;
inline constexpr u32 kGatewayClientEventCapacity = 256;
// Ordinary typed records, the mailbox ring and the UI pop scratch.
inline constexpr u32 kGatewayClientDispatchPayloadCapacity = 128 * 1024;
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
    u8 reserved[3];
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
    // The current member's GuildSelfMember bits (with guild_settings_self_member).
    u8 self_member;
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

GatewayEventDecodeResult gateway_client_decode_presence(JsonValue data, Slice<u32> scratch,
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
    // (user_activity.hpp), or the presence listed none.
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
struct GatewayClientNotificationThreadRecord {
    MessageNotificationThreadSetting thread;
    u32 flags, known_gates, gates, reserved;
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

bool gateway_client_notification_entry(const GatewayClientEvent&, ConstSlice<u8>,
                                       MessageNotificationGuildSetting*) noexcept;
bool gateway_client_notification_override_at(const GatewayClientEvent&, ConstSlice<u8>, u32,
                                             MessageNotificationOverride*) noexcept;
bool gateway_client_notification_sidecar(ConstSlice<u8>, GatewayClientNotificationSidecar*,
                                         ConstSlice<u8>*) noexcept;
bool gateway_client_notification_thread_at(ConstSlice<u8>, u32,
                                           GatewayClientNotificationThreadRecord*) noexcept;
bool gateway_client_notification_trim(const GatewayClientEvent&, ConstSlice<u8>, GatewayClientEvent*,
                                      ConstSlice<u8>*) noexcept;
struct GatewayClientDispatchRouteResult;
// Same two-pass READY producer used by the outer preflight. Policy
// failure emits incomplete plus nonarming completion, never transport failure.
GatewayClientDispatchRouteResult gateway_client_publish_notification_snapshot(
    const GatewayReady&, Slice<u32> array_scratch, Slice<u32> object_scratch, Slice<char> payload_scratch,
    u64 sequence, u64 now_ms, bool (*publish)(void*, const GatewayClientEvent&, ConstSlice<u8>) noexcept,
    void* context) noexcept;

struct GatewayClientDispatchRouteResult {
    GatewayEventDecodeError event_error;
    GatewayClientDispatchRouteError error;
    JsonScanError scan_error;
    JsonReadError read_error;
    u32 error_offset;
    u32 detail;
    // A state record failed only because its payload scratch was full; the
    // same record may fit a larger scratch.
    bool payload_overflow;

    constexpr bool ok() const noexcept {
        return error == GatewayClientDispatchRouteError::none;
    }
};

// Pointer-free single relationship record. Cursor advances only after complete
// validation; the payload may batch records without keeping JSON pointers.
bool gateway_client_relationship_record(ConstSlice<u8> payload, u32* cursor,
                                        RelationshipMutation* mutation) noexcept;
using GatewayRelationshipPublish = bool (*)(void* context, const GatewayClientEvent& event,
                                            ConstSlice<u8> payload) noexcept;
// Used by the READY worker and by credential-free queue fixtures. A
// completion is queued only after every relationship and initial presence
// record validates and is queued. Consumers stage rows until that completion.
GatewayClientDispatchRouteResult gateway_client_publish_relationship_snapshot(
    JsonValue relationships, JsonValue merged_presences, Slice<u32> array_scratch,
    Slice<u32> object_scratch, u64 sequence, u64 now_ms, GatewayRelationshipPublish publish,
    void* context) noexcept;
enum class GatewayVoiceStateSource : u8 {
    guild,  // READY guild or GUILD_CREATE object: replaces guild_id's states.
    update, // VOICE_STATE_UPDATE data.
    batch,  // VOICE_STATE_UPDATE_BATCH data.
};
// Queues session-free voice_state_snapshot chunks, preceded by user_snapshot
// batches for embedded member users. Every row validates before anything is
// queued; malformed or oversized documents are skipped. Returns false only
// when queueing fails. payload must hold kGatewayClientDispatchPayloadCapacity.
// `located`: the guild's voice_states value from gateway_guild_arrays_find,
// so a guild source is not scanned again (absent: located->kind is not array).
bool gateway_client_publish_voice_states(JsonValue data, GatewayVoiceStateSource source, u64 guild_id,
                                         Slice<u32> scratch, Slice<char> payload, u64 sequence,
                                         u64 now_ms, GatewayRelationshipPublish publish,
                                         void* context, const JsonValue* located = nullptr) noexcept;
// PASSIVE_UPDATE_V2: the server sends voice joins, moves and leaves for guilds the
// session does not actively subscribe as updated_voice_states/removed_voice_states
// instead of VOICE_STATE_UPDATE. updated_members users precede the rows.
bool gateway_client_publish_passive_voice_states(JsonValue data, Slice<u32> scratch, Slice<char> payload,
                                                 u64 sequence, u64 now_ms, GatewayRelationshipPublish publish,
                                                 void* context) noexcept;
// READY guilds and GUILD_CREATE carry stage_instances[]: one replace snapshot
// for guild_id when the array is present (malformed rows are skipped).
bool gateway_client_publish_stage_instances(JsonValue guild, u64 guild_id, Slice<u32> scratch, Slice<char> payload,
                                            u64 sequence, u64 now_ms, GatewayRelationshipPublish publish,
                                            void* context, const JsonValue* located = nullptr) noexcept;
// READY guilds and GUILD_CREATE carry guild_scheduled_events[]: one verbatim
// guild_scheduled_event_snapshot when the array is present and fits.
bool gateway_client_publish_scheduled_events(JsonValue guild, u64 guild_id, Slice<u32> scratch, Slice<char> payload,
                                             u64 sequence, u64 now_ms, GatewayRelationshipPublish publish,
                                             void* context, const JsonValue* located = nullptr) noexcept;
// READY guilds and GUILD_CREATE carry stickers[]: one verbatim
// guild_stickers_update when the list is present and fits.
bool gateway_client_publish_guild_stickers(JsonValue guild, u64 guild_id, Slice<u32> scratch, Slice<char> payload,
                                           u64 sequence, u64 now_ms, GatewayRelationshipPublish publish,
                                           void* context) noexcept;
// The three ancillary arrays of a READY guild or GUILD_CREATE object, found in
// one scan and visit (the last duplicate key wins, as in each publisher).
struct GatewayGuildArrays {
    JsonValue voice_states;
    JsonValue stage_instances;
    JsonValue scheduled_events;
};
bool gateway_guild_arrays_find(JsonValue guild, Slice<u32> scratch, GatewayGuildArrays* arrays) noexcept;
// STAGE_INSTANCE_CREATE/UPDATE/DELETE as a one-record stage_instance_snapshot.
// A malformed instance routes as unknown (never a session failure).
GatewayClientDispatchRouteResult gateway_client_route_stage_instance(const GatewayEnvelope& envelope,
                                                                     Slice<u32> scratch, u32 maximum_depth,
                                                                     Slice<char> payload_scratch,
                                                                     GatewayClientEvent* event,
                                                                     ConstSlice<u8>* payload) noexcept;
// READY_SUPPLEMENTAL guilds[] carry the initial voice_states of every available
// guild (READY guilds do not). Each listed guild replaces its states; entries
// without voice_states are skipped. array_scratch indexes the whole document and
// then the guild list; it must stay disjoint from object_scratch, which indexes
// each guild and its rows.
bool gateway_client_publish_supplemental_voice_states(JsonValue data, Slice<u32> array_scratch,
                                                      Slice<u32> object_scratch, Slice<char> payload,
                                                      u64 sequence, u64 now_ms,
                                                      GatewayRelationshipPublish publish, void* context) noexcept;
GatewayClientDispatchRouteResult gateway_client_publish_friend_presences(
    JsonValue merged_presences, Slice<u32> array_scratch, Slice<u32> object_scratch, u64 sequence,
    u64 now_ms, GatewayRelationshipPublish publish, void* context) noexcept;
// READY_SUPPLEMENTAL merged_presences.guilds, aligned with its guilds array:
// each presence that carries activities or a custom status is published as a
// PRESENCE_UPDATE of its guild; the rest add nothing the member list lacks.
// array_scratch indexes the whole document, object_scratch each presence.
// Malformed rows are skipped; only a full mailbox fails.
GatewayClientDispatchRouteResult gateway_client_publish_guild_presences(
    JsonValue data, Slice<u32> array_scratch, Slice<u32> object_scratch, u64 sequence, u64 now_ms,
    GatewayRelationshipPublish publish, void* context) noexcept;

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

struct GatewayClient;
struct RestClient;
struct RestClientResponse;
struct RestClientSubmitResult;
enum class GatewaySelfStatusError : u8 {
    none,
    unavailable,
    settings,
    transport,
    rejected,
    out_of_date
};
struct GatewayClientSelfStatusSnapshot {
    u64 profile_id, self_user_id, revision, settings_revision;
    GatewayUserStatus status, confirmed_status;
    GatewaySelfStatusError last_error;
    bool available, authoritative, pending;
};
GatewayClientSelfStatusSnapshot gateway_client_self_status_snapshot(const GatewayClient*) noexcept;
RestClientSubmitResult gateway_client_self_status_submit(GatewayClient*, RestClient*,
                                                         u64 profile_id, u64 self_user_id,
                                                         SelfStatusSettingsOperation operation,
                                                         GatewayUserStatus status, u64 request_id,
                                                         u64 unix_now_ms) noexcept;
bool gateway_client_self_status_complete(GatewayClient*, u64 profile_id, u64 self_user_id, u64 request_id,
                                         const RestClientResponse*, Utf8View response_body) noexcept;
// Custom status (StatusSettings field 2) of the observed settings group; false
// while the group is not authoritative.
bool gateway_client_custom_status_snapshot(const GatewayClient*, SelfCustomStatus* output) noexcept;
// Replaces (null clears) the custom status with a settings-proto PATCH that
// keeps the rest of the status group. Completes through self_status_complete.
RestClientSubmitResult gateway_client_custom_status_submit(GatewayClient*, RestClient*,
                                                           u64 profile_id, u64 self_user_id,
                                                           const SelfCustomStatus*, u64 request_id) noexcept;
// "Share my activity" (StatusSettings.show_current_game) as last observed.
struct GatewayActivitySharing {
    bool known, share;
    u8 reserved[6];
};
static_assert(sizeof(GatewayActivitySharing) == 8);
// Local presence owned by the UI (automatic idle, shared game). A non-null
// input replaces it and republishes opcode 3 when it changed; the game is
// sent only while sharing is on and the status is not invisible. A non-null
// output receives the sharing setting. Returns false for invalid input.
bool gateway_client_local_presence(GatewayClient*, const GatewayLocalPresence* input,
                                   GatewayActivitySharing* output) noexcept;
// Wakes the worker after the client's focus changed in its session metadata.
void gateway_client_activity_changed(GatewayClient*) noexcept;
// Sets show_current_game with a settings-proto PATCH that keeps the rest of
// the status group. Completes through self_status_complete.
RestClientSubmitResult gateway_client_activity_sharing_submit(GatewayClient*, RestClient*,
                                                              u64 profile_id, u64 self_user_id, bool share,
                                                              u64 request_id) noexcept;
AccountRequirement gateway_client_required_action(const GatewayClient* client) noexcept;

struct GatewayClient {
    NetworkWorker worker;
    void* runtime;
    HANDLE state_event;
    HANDLE notification_event;
    volatile LONG phase;
    volatile LONG error;
    volatile LONG native_error;
    volatile LONG close_code;
    volatile LONG required_action;
    volatile LONG64 hello_elapsed_us;
    volatile LONG64 frecency_settings_revision;
};

// Resume state is profile-bound and contains only the one user session.
struct GatewayUserSessionSnapshot {
    GatewayResumeState resume;
    u64 profile_id;
    // Authenticated READY identity, separate from the local profile key.
    // Zero means unknown in a legacy snapshot; never infer it from a token.
    u64 self_user_id;
    bool has_resume;
};

// Normal-user path. A successful start consumes the complete normal_user
// profile and binds the WebSocket to the v9 ETF zstd-stream endpoint policy;
// commands go out as ETF.
// Invalid or failed synchronous startup leaves the profile unchanged.
GatewayClientResult gateway_client_start_user(GatewayClient* client, AccountProfile* profile,
                                              const GatewayUserClientConfiguration* configuration,
                                              const GatewayEndpoint* gateway_endpoint,
                                              const GatewayUserSessionSnapshot* session) noexcept;
GatewayClientResult gateway_client_stop(GatewayClient* client, u32 timeout_milliseconds) noexcept;
// User-only counterpart. After the worker finishes, the profile metadata
// and locked credential are returned together. A client that is not a user
// one is rejected without stopping and the empty output remains unchanged.
GatewayClientResult gateway_client_stop_return_user_profile(GatewayClient* client, u32 timeout_milliseconds,
                                                            AccountProfile* profile) noexcept;
GatewayClientResult gateway_client_status(const GatewayClient* client) noexcept;
// Observer APIs are UI-thread operations. They must not race
// gateway_client_stop(), which destroys the worker-owned runtime after joining.
bool gateway_client_pop_event(GatewayClient* client, GatewayClientEvent* event) noexcept;
GatewayClientPopResult gateway_client_pop_event_payload(GatewayClient* client, GatewayClientEvent* event,
                                                        Slice<u8> payload_output) noexcept;
HANDLE gateway_client_notification_handle(const GatewayClient* client) noexcept;
u32 gateway_client_dropped_events(const GatewayClient* client) noexcept;
bool gateway_client_snapshot_user_session(const GatewayClient* client,
                                          GatewayUserSessionSnapshot* session) noexcept;
// Replaces the desired visible guild/channel subscription. The worker keeps
// the latest revision across reconnects and transmits it with opcode
// 37 once the authenticated session is READY.
bool gateway_client_subscribe_guild_members(GatewayClient* client,
                                            const GatewayGuildSubscription& subscription) noexcept;
bool gateway_client_request_guild_members(GatewayClient*, const GatewayGuildMemberRequest&) noexcept;
// Queues Request Channel Info (opcode 43) for a guild's voice channel statuses
// and start times; false when it was already asked this session or is queued.
bool gateway_client_request_channel_info(GatewayClient*, u64 guild_id) noexcept;
void gateway_client_cancel_guild_member_request(GatewayClient*) noexcept;
// Retires only name-query interest, including server-rejected replay authority.
void gateway_client_cancel_guild_member_query(GatewayClient*) noexcept;
inline constexpr u32 kGatewayVoiceSpeakingUsers = 16;
struct GatewayClientVoiceSnapshot {
    u64 user_id, guild_id, channel_id, generation;
    u32 flags, present_flags;
    VoiceSetupPhase phase;
    GatewayVoiceError error;
    bool awaiting_acknowledgement;
    VoiceSocketPhase socket_phase;
    VoiceConnectionError connection_error;
    bool identify_submitted;
    u16 voice_close_code;
    VoiceUdpPhase udp_phase;
    VoiceUdpError udp_error;
    // Remote users currently heard speaking (received audio energy, or packet
    // activity with op 5 flags while deafened), sorted by ID.
    u32 speaking_count;
    u64 speaking_users[kGatewayVoiceSpeakingUsers];
    constexpr bool operator==(const GatewayClientVoiceSnapshot&) const noexcept = default;
};
static_assert(sizeof(GatewayClientVoiceSnapshot) == 184); // Crosses the account module ABI.
// The channel of the live call, guild or DM (guild 0), or 0 when there is none.
constexpr u64 gateway_voice_call_channel(const GatewayClientVoiceSnapshot& voice) noexcept {
    return voice.phase != VoiceSetupPhase::idle && voice.error == GatewayVoiceError::none ? voice.channel_id : 0;
}
// True while the voice worker lists the user among the call's speakers: it
// does as soon as it sends speech.
constexpr bool gateway_voice_self_speaking(const GatewayClientVoiceSnapshot& voice) noexcept {
    for (u32 i = 0; voice.user_id && i < voice.speaking_count && i < kGatewayVoiceSpeakingUsers; ++i)
        if (voice.speaking_users[i] == voice.user_id)
            return true;
    return false;
}
inline constexpr u32 kGatewayVoiceVideoUsers = 16;
// A remote participant announcing an active camera or screen stream (op 12):
// the UI's "video available" flag. frames stays zero until media arrives.
struct GatewayVoiceVideoUser {
    u64 user_id;
    u32 ssrc, frames;
    VoiceSourceKind kind; // video (camera) or screen.
    u8 quality;
    u8 reserved[6];
    constexpr bool operator==(const GatewayVoiceVideoUser&) const noexcept = default;
};
struct GatewayClientVoiceVideoSnapshot {
    u64 generation; // Voice setup generation the list belongs to.
    u32 count;      // Users sorted by ID.
    VoiceVideoCodec codec;
    u8 reserved[3];
    GatewayVoiceVideoUser users[kGatewayVoiceVideoUsers];
    u64 frames_delivered, frames_rejected; // Decoder hand-offs; DAVE rejections.
    // Integers without padding (asserted below): one byte compare instead of one per listed user.
    bool operator==(const GatewayClientVoiceVideoSnapshot& other) const noexcept {
        return bytes_equal(this, &other, sizeof(*this));
    }
};
static_assert(sizeof(GatewayVoiceVideoUser) == 24 && sizeof(GatewayClientVoiceVideoSnapshot) == 416); // ABI.
static_assert(__has_unique_object_representations(GatewayClientVoiceVideoSnapshot));
// UI-thread APIs, with the same stop/join exclusion as the observer APIs above.
// No capture is enabled here. Credentials-ready is not a connected voice call.
bool gateway_client_request_voice_state(GatewayClient* client,
                                        const GatewayVoiceStateUpdate& state) noexcept;
bool gateway_client_voice_snapshot(const GatewayClient* client,
                                   GatewayClientVoiceSnapshot* output) noexcept;
// Local playback volume (0-200 %) and mute for one remote user; applies
// immediately to the mix. Not sent to the server and not persisted here.
bool gateway_client_voice_user_audio(GatewayClient* client, u64 user_id, u32 gain_percent, bool muted) noexcept;
// Microphone, output and input-mode settings; the worker applies the latest.
bool gateway_client_voice_audio_configure(GatewayClient* client, const VoiceAudioSettings& settings) noexcept;
bool gateway_client_voice_audio_status(GatewayClient* client, VoiceAudioStatus* output) noexcept;
bool gateway_client_voice_video_snapshot(const GatewayClient* client,
                                         GatewayClientVoiceVideoSnapshot* output) noexcept;
// Stop (false) or resume (true) receiving one remote user's video: media sink
// wants (op 15) quality 0 for that user's video SSRCs. Kept for the call.
bool gateway_client_voice_video_wanted(GatewayClient* client, u64 user_id, bool wanted) noexcept;
// Where assembled, DAVE-opened call video goes (the stream decoders). The
// worker installs the sink on its next voice drive and hands bind_keyframes
// the thread-safe keyframe requester of this client's video receive path;
// gateway stop ends every stream and binds an empty requester before the
// runtime is freed. The sink context must outlive the client's runtime.
struct GatewayVideoFrames {
    VideoFrameSink sink;
    void* context;
    void (*bind_keyframes)(void* context, VideoKeyframeRequester requester) noexcept;
};
bool gateway_client_set_video_frames(GatewayClient* client, const GatewayVideoFrames& frames) noexcept;
// True when the current runtime already holds frames whose context is `context`.
bool gateway_client_has_video_frames(const GatewayClient* client, const void* context) noexcept;
// Installs the soundboard mixer into this runtime's call playback (a new
// session starts without one; null detaches). The mixer must outlive the
// runtime. Returns true while call audio is live (media attached).
bool gateway_client_attach_soundboard(GatewayClient* client, struct SoundboardMixer* mixer) noexcept;
// Go Live watch (UI thread). Watching requires the local call in the key's
// voice channel; one stream at a time (watching another key deletes the
// previous one). A key with type none stops watching. The worker sends
// STREAM_WATCH, opens the stream's own voice connection and ends it on
// STREAM_DELETE, stop, or when the call leaves the channel.
bool gateway_client_stream_watch(GatewayClient* client, const GatewayStreamKey& key) noexcept;
bool gateway_client_stream_snapshot(const GatewayClient* client, GatewayClientStreamSnapshot* output) noexcept;
// Stream audio bus volume (0-200 %, default 100), on top of the output volume.
bool gateway_client_stream_volume(GatewayClient* client, u32 percent) noexcept;
u32 gateway_client_runtime_bytes() noexcept;
// UI-thread request; transport teardown and fresh identify run on the worker.
GatewayStateRecoveryResult gateway_client_request_fresh_state(GatewayClient* client) noexcept;
u32 gateway_client_user_runtime_bytes() noexcept;

// Pure identity gate shared by the worker and deterministic tests. The
// normal-user contract is v9 and unsharded.
bool gateway_client_ready_matches_identity(GatewayClientIdentity identity,
                                           const GatewayReady& ready) noexcept;

// Shared unread-channel normalization. Staging storage may be touched on error;
// the descriptor and payload are committed only after the complete array validates.
GatewayClientDispatchRouteResult gateway_client_route_channel_cursors(const GatewayEnvelope& envelope,
    Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> storage,
    GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept;
bool gateway_client_channel_cursor_record(ConstSlice<u8> payload, u32 index,
    GatewayClientChannelCursor* output) noexcept;
// Deterministic worker-side routing boundary. It decodes supported message
// mutations while the Gateway envelope views are still valid. Returned payload
// bytes point into content_scratch or bulk_ids and must be copied before reuse.
GatewayClientDispatchRouteResult gateway_client_route_dispatch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> content_scratch, Slice<u64> bulk_ids, u64 self_user_id, GatewayClientEvent* event,
    ConstSlice<u8>* payload) noexcept;
inline GatewayClientDispatchRouteResult gateway_client_route_dispatch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> content_scratch, Slice<u64> bulk_ids, GatewayClientEvent* event,
    ConstSlice<u8>* payload) noexcept {
    return gateway_client_route_dispatch(envelope, structural_scratch, maximum_depth, content_scratch,
                                         bulk_ids, 0, event, payload);
}
// The pointer-free marker queued in place of a dispatch its route rejected:
// error metadata, the recovery kind its event family needs, and for the
// remaining (unknown) kind the event name. Never payload contents.
void gateway_client_route_failure_event(const GatewayEnvelope& envelope,
                                        const GatewayClientDispatchRouteResult& routed, u64 now_ms,
                                        GatewayClientEvent* event) noexcept;
// Supplies `bytes` of committed storage for one oversized record, at a fixed
// base address. It may wait for the consumer to release the previous one.
struct GatewayClientLargeRecord {
    bool (*acquire)(void* context, u32 bytes, Slice<char>* storage) noexcept;
    void* context;
};
// gateway_client_route_dispatch, except that a state record overflowing
// content_scratch is routed again into growing spans (256 KiB doubling to
// kGatewayClientGuildRecordCapacity) of the large record storage. The payload
// then points into that storage.
GatewayClientDispatchRouteResult gateway_client_route_dispatch_large(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> content_scratch, Slice<u64> bulk_ids, u64 self_user_id, GatewayClientLargeRecord large,
    GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept;
GatewayClientDispatchRouteResult gateway_client_route_state_dispatch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> payload_scratch, u64 self_user_id, GatewayClientEvent* event, ConstSlice<u8>* payload,
    bool* handled) noexcept;
// Projects a REST channel receipt with the CHANNEL_CREATE rules. Keeps event metadata.
GatewayClientDispatchRouteResult gateway_client_route_channel_create(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> storage,
    u64 self_user_id, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept;
GatewayClientDispatchRouteResult gateway_client_route_private_channel(
    JsonValue data, Slice<u32> structural_scratch, u32 maximum_depth, Slice<char> storage,
    GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept;
GatewayClientDispatchRouteResult gateway_client_route_member_list_dispatch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> payload_scratch, GatewayClientEvent* event, ConstSlice<u8>* payload,
    bool* handled) noexcept;
// GUILD_MEMBER_REMOVE and GUILD_BAN_ADD/REMOVE: guild ID plus one user record.
GatewayClientDispatchRouteResult gateway_client_route_guild_admin_dispatch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> payload_scratch, GatewayClientEvent* event, ConstSlice<u8>* payload,
    bool* handled) noexcept;
// Validates the exact single-record payload; name prefers the display name.
bool gateway_client_event_guild_membership(const GatewayClientEvent& event, ConstSlice<u8> payload, u64* guild_id,
                                           u64* user_id, Utf8View* name) noexcept;
GatewayClientDispatchRouteResult gateway_client_route_thread_member_dispatch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> payload_scratch, GatewayClientEvent* event, ConstSlice<u8>* payload,
    bool* handled) noexcept;
GatewayClientDispatchRouteResult gateway_client_route_thread_member_dispatch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> payload_scratch, u64 self_user_id, GatewayClientEvent* event, ConstSlice<u8>* payload,
    bool* handled) noexcept;
bool gateway_client_event_thread_member_record(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                               u32* cursor, u32* index,
                                               GatewayClientMemberListMutation* mutation,
                                               ConstSlice<u8>* role_payload, Utf8View* nickname,
                                               Utf8View* display_name, Utf8View* avatar_hash,
                                               Utf8View* username, ConstSlice<u8>* user_appearance,
                                               ConstSlice<u8>* member_appearance) noexcept;
GatewayClientDispatchRouteResult gateway_client_route_ready(
    const GatewayReady& ready, Slice<u32> structural_scratch, Slice<u32> object_scratch, u32 maximum_depth,
    Slice<char> payload_scratch, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept;
// Fallback for a full guild object whose typed route failed: reads only its
// "id" and produces an unavailable GUILD_CREATE (the guild stays listed and
// shows as unavailable). False when no single valid id can be read.
bool gateway_client_route_guild_placeholder(JsonValue guild, Slice<u32> structural_scratch,
                                            u32 maximum_depth, GatewayClientEvent* event) noexcept;
bool gateway_client_event_as_ready_guilds(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                          Slice<u64> guild_ids, u32* guild_count) noexcept;
bool gateway_client_event_state_name(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                     Utf8View* name) noexcept;
bool gateway_client_event_guild_icon(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                     Utf8View* icon_hash) noexcept;
// Guild create/update payloads lead with the name, icon and banner strings.
constexpr u32 gateway_client_guild_banner_bytes(const GatewayClientStateMutation& state) noexcept {
    return state.guild_banner ? state.guild_banner - 1u : 0u;
}
constexpr u32 gateway_client_guild_strings_bytes(const GatewayClientStateMutation& state) noexcept {
    return static_cast<u32>(state.name_bytes) + state.icon_bytes + gateway_client_guild_banner_bytes(state) +
           state.guild_details_bytes;
}
// The banner hash (empty for a null banner); false when the event carried no
// banner key or the payload is malformed.
bool gateway_client_event_guild_banner(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                       Utf8View* banner_hash) noexcept;
// Server details after the banner: this header, then the description, the
// vanity code and the feature names. Each name is one length byte and its
// UTF-8 bytes, in the order the server sent them. Fields are valid only
// where `known` (GuildDetailField) has their bit.
struct GatewayGuildDetails {
    u64 rules_channel_id;
    u64 public_updates_channel_id;
    u64 safety_alerts_channel_id;
    // incidents_data, Unix milliseconds; zero when absent or null.
    u64 invites_disabled_until_ms;
    u64 dms_disabled_until_ms;
    u64 raid_detected_at_ms;
    u64 dm_spam_detected_at_ms;
    u32 system_channel_flags;
    u32 premium_subscription_count;
    u32 member_count;
    u16 known;
    u16 description_bytes;
    u16 feature_names_bytes;
    u16 feature_count;
    u8 vanity_bytes;
    u8 premium_tier;
    // incidents_data lockdown_duration_hours, saturated to 255.
    u8 lockdown_duration_hours;
    bool premium_progress_bar_enabled;
    // The invite splash hash; empty for none.
    u8 splash_bytes;
    char splash[34];
};
static_assert(sizeof(GatewayGuildDetails) == 120);
// False when the event carries no details record or it is malformed.
bool gateway_client_event_guild_details(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                        GatewayGuildDetails* details, Utf8View* description,
                                        Utf8View* vanity_url_code, ConstSlice<u8>* feature_names) noexcept;
bool gateway_client_event_next_guild_channel(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                             u32* cursor, u32* index, GatewayClientStateMutation* channel,
                                             Utf8View* name, ConstSlice<u8>* overwrite_payload) noexcept;
inline bool gateway_client_event_next_guild_channel(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                                    u32* cursor, u32* index,
                                                    GatewayClientStateMutation* channel,
                                                    Utf8View* name) noexcept {
    ConstSlice<u8> overwrites{};
    return gateway_client_event_next_guild_channel(event, payload, cursor, index, channel, name,
                                                   &overwrites);
}
bool gateway_client_event_guild_role(const GatewayClientEvent& event, ConstSlice<u8> payload, u32 index,
                                     GatewayClientRoleMutation* role, Utf8View* name) noexcept;
// Transient offsets into a typed packet; no role/name copies.
// Used output must not overlap event/payload. A failed scan may leave a partial
// output prefix; only a successful table may be used with guild_role_at.
bool gateway_client_event_guild_role_offsets(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                             Slice<u32> offsets) noexcept;
// offset must come from successful guild_role_offsets for this same packet.
bool gateway_client_event_guild_role_at(const GatewayClientEvent& event, ConstSlice<u8> payload, u32 offset,
                                        GatewayClientRoleMutation* role, Utf8View* name) noexcept;
inline bool gateway_client_event_guild_role(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                            u32 index, GatewayClientRoleMutation* role) noexcept {
    Utf8View name{};
    return gateway_client_event_guild_role(event, payload, index, role, &name);
}
bool gateway_client_event_guild_emojis_begin(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                             u32* begin) noexcept;
bool gateway_client_event_next_guild_emoji(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                           u32* cursor, u32* index, GatewayClientEmojiMutation* emoji,
                                           ConstSlice<u8>* role_payload, Utf8View* name) noexcept;
bool gateway_client_event_channel_overwrites(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                             ConstSlice<u8>* overwrite_payload) noexcept;
bool gateway_client_permission_overwrite_at(ConstSlice<u8> overwrite_payload, u32 overwrite_count,
                                            u32 index,
                                            GatewayClientPermissionOverwriteMutation* overwrite) noexcept;
bool gateway_client_event_member_list_record(
    const GatewayClientEvent& event, ConstSlice<u8> payload, u32* cursor, u32* index,
    GatewayClientMemberListMutation* mutation, ConstSlice<u8>* role_payload, Utf8View* nickname,
    Utf8View* display_name, Utf8View* avatar_hash, Utf8View* username = nullptr,
    ConstSlice<u8>* user_appearance = nullptr, ConstSlice<u8>* member_appearance = nullptr) noexcept;
bool gateway_client_event_as_message_poll_vote(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                               GatewayMessagePollVoteEvent* output) noexcept;
bool gateway_client_event_as_message(const GatewayClientEvent& event, const ConstSlice<u8>& payload,
                                     GatewayMessageEvent* message) noexcept;
bool gateway_client_event_as_message(const GatewayClientEvent& event, const ConstSlice<u8>& payload,
                                     const GatewayMessageRichScratch& rich_scratch,
                                     GatewayMessageEvent* message) noexcept;
// Reads only the copied scalar rich header; no rich descriptor arrays or JSON
// storage are materialized. A valid message without an interaction returns 0.
bool gateway_client_event_message_interaction_user(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                                   u64* user_id) noexcept;
// Mention roles remain little-endian packed after message.content_bytes; the
// reconstructed GatewayMessageEvent carries no unaligned u64 view.
bool gateway_client_event_message_mention_role_at(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                                  u32 index, u64* role_id) noexcept;
bool gateway_client_event_message_member_role_at(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                                 u32 index, u64* role_id) noexcept;
bool gateway_client_event_message_author_name(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                              Utf8View* name) noexcept;
bool gateway_client_event_message_author_avatar_hash(const GatewayClientEvent& event,
                                                     ConstSlice<u8> payload,
                                                     Utf8View* avatar_hash) noexcept;
// The author's server nickname; empty when the member has none.
bool gateway_client_event_message_member_nick(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                              Utf8View* nick) noexcept;
bool gateway_client_event_message_mention_user_at(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                                  u32 index, u64* user_id, Utf8View* name) noexcept;
// Validate the complete snapshot user list before the first synchronous call.
// Stop at the first false callback. Names borrow payload storage. The callback
// must not modify or invalidate the event or payload during this visit.
bool gateway_client_event_visit_message_snapshot_mention_users(
    const GatewayClientEvent& event, ConstSlice<u8> payload, void* context,
    bool (*visit)(void*, u64, Utf8View) noexcept) noexcept;
// Recipient add/remove system rows retain their subject (mentions[0]). The
// decoded event leaves mention_users empty; this borrows the first mention
// into storage for those types (a no-op otherwise).
inline bool gateway_client_event_message_system_target(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                                       GatewayMessageEvent* message,
                                                       GatewayMessageNamedUser* storage) noexcept {
    if (!message || !storage || (message->type != 1 && message->type != 2) || !event.message.mention_user_count ||
        message->mention_users.count)
        return true;
    u64 id = 0;
    Utf8View name{};
    if (!gateway_client_event_message_mention_user_at(event, payload, 0, &id, &name))
        return false;
    *storage = {id, name};
    message->mention_users = {storage, 1};
    return true;
}
bool gateway_client_event_as_message_delete(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                            GatewayMessageDeleteEvent* message) noexcept;
bool gateway_client_event_as_message_bulk_delete(const GatewayClientEvent& event, const ConstSlice<u8>& payload,
                                                 const Slice<u64>& ids,
                                                 GatewayMessageBulkDeleteEvent* message) noexcept;
bool gateway_client_event_as_message_reaction(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                              GatewayMessageReactionEvent* reaction) noexcept;
bool gateway_client_event_as_typing_start(const GatewayClientEvent& event, ConstSlice<u8> payload,
                                          GatewayTypingStartEvent* typing) noexcept;

} // namespace maicord
