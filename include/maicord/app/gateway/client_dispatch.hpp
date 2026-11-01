#pragma once
#include "maicord/app/gateway/client_event.hpp"
#include "maicord/model/notifications/message_notifications.hpp"
#include "maicord/protocol/gateway/typing_events.hpp"

namespace maicord {

struct GatewayClientNotificationThreadRecord {
    MessageNotificationThreadSetting thread;
    u32 flags, known_gates, gates, reserved;
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
// message_scratch (app/chat/messages/event_scratch.hpp) is the storage MESSAGE_CREATE/UPDATE decode into; a message
// dispatch routed without one fails as invalid input, every other event needs none.
struct GatewayMessageDecodeScratch;
GatewayClientDispatchRouteResult gateway_client_route_dispatch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> content_scratch, Slice<u64> bulk_ids, u64 self_user_id, GatewayClientEvent* event,
    ConstSlice<u8>* payload, GatewayMessageDecodeScratch* message_scratch = nullptr) noexcept;
inline GatewayClientDispatchRouteResult gateway_client_route_dispatch(
    const GatewayEnvelope& envelope, Slice<u32> structural_scratch, u32 maximum_depth,
    Slice<char> content_scratch, Slice<u64> bulk_ids, GatewayClientEvent* event, ConstSlice<u8>* payload,
    GatewayMessageDecodeScratch* message_scratch = nullptr) noexcept {
    return gateway_client_route_dispatch(envelope, structural_scratch, maximum_depth, content_scratch,
                                         bulk_ids, 0, event, payload, message_scratch);
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
    GatewayClientEvent* event, ConstSlice<u8>* payload,
    GatewayMessageDecodeScratch* message_scratch = nullptr) noexcept;
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
