#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/upload_allowance.hpp"
#include "maicord/model/content_policy.hpp"

namespace maicord {
struct ReadStateStore;
struct ChatStateStore;

inline constexpr u32 kChannelObfuscatedFlag = 1u << 17;
inline constexpr u8 kChannelContentAgeRestricted = 1u << 0;
inline constexpr u32 kGuildRoleMaximum = 256;
inline constexpr u32 kGuildIconHashMaximumBytes = 34;
inline constexpr u32 kGuildEmojiMaximum = 512;
inline constexpr u32 kCustomEmojiNameMaximumBytes = 32;

enum ChannelAction : u32 {
    channel_action_view = 1u << 0,
    channel_action_read_history = 1u << 1,
    channel_action_send_messages = 1u << 2,
    channel_action_add_reactions = 1u << 3,
    channel_action_embed_links = 1u << 4,
    channel_action_attach_files = 1u << 5,
    channel_action_mention_everyone = 1u << 6,
    channel_action_manage_messages = 1u << 7,
    channel_action_manage_channel = 1u << 8,
    channel_action_manage_threads = 1u << 9,
    channel_action_create_public_threads = 1u << 10,
    channel_action_create_private_threads = 1u << 11,
    channel_action_connect = 1u << 12,
    channel_action_speak = 1u << 13,
    channel_action_stream = 1u << 14,
    channel_action_move_members = 1u << 15,
    channel_action_send_voice_messages = 1u << 16,
    channel_action_pin_messages = 1u << 17,
};

enum class GuildMutationKind : u8 {
    create,
    update,
    unavailable,
    remove,
};

enum GuildSettingsField : u16 {
    guild_settings_mfa_level = 1u << 0,
    guild_settings_verification_level = 1u << 1,
    guild_settings_default_notifications = 1u << 2,
    guild_settings_explicit_content_filter = 1u << 3,
    guild_settings_system_channel = 1u << 4,
    guild_settings_afk_channel = 1u << 5,
    guild_settings_afk_timeout = 1u << 6,
    guild_settings_self_timeout = 1u << 7,
    // The current member's pending and onboarding state (GuildSelfMember).
    guild_settings_self_member = 1u << 8,
    // Quarantine is independent of pending/onboarding partial observations.
    guild_settings_self_quarantine = 1u << 9,
    guild_settings_all = 0x3FFu,
};
// GuildSettings::self_member bits, from the member object's pending flag and
// member flags for onboarding and AutoMod quarantine.
enum GuildSelfMember : u8 {
    guild_self_member_pending = 1u << 0,
    guild_self_member_onboarding_completed = 1u << 1,
    guild_self_member_onboarding_started = 1u << 2,
    // Previewing (lurking): a live GUILD_CREATE without a join time and
    // without the current member. A later member update replaces it.
    guild_self_member_lurking = 1u << 3,
    guild_self_member_quarantined = 1u << 4,
};
// Active AutoMod name and guild-tag quarantine flags. The retired bio flag
// does not quarantine a member.
constexpr bool guild_member_flags_quarantined(u32 flags) noexcept {
    return (flags & ((1u << 7) | (1u << 10))) != 0;
}
// Community features carried in GuildRecord::upload_features above the
// upload-size bits (1, 2, 4), from the guild's features array, and
// ANIMATED_BANNER (an a_ banner animates only with it).
enum GuildCommunityFeature : u8 {
    guild_record_community = 1u << 3,
    guild_record_onboarding = 1u << 4,
    guild_record_verification_gate = 1u << 5,
    guild_record_onboarding_prompts = 1u << 6,
    guild_record_animated_banner = 1u << 7,
};
// Rules hold the composer while the member is pending in a server that
// screens members (onboarding servers clear pending through onboarding).
constexpr bool guild_rules_gate(u8 features, u8 self_member, bool self_member_known) noexcept {
    return self_member_known && (self_member & guild_self_member_pending) &&
           ((features & guild_record_verification_gate) || !(features & guild_record_onboarding));
}

// Partial observation; only fields in `known` replace stored values.
struct GuildSettings {
    u64 system_channel_id;
    u64 afk_channel_id;
    u32 afk_timeout_seconds;
    // Unix seconds; zero when the current account is not timed out.
    u32 self_communication_disabled_until;
    u16 known;
    u8 mfa_level;
    u8 verification_level;
    u8 default_message_notifications;
    u8 explicit_content_filter;
    u8 self_member;
};
constexpr bool guild_self_quarantined(const GuildSettings& settings) noexcept {
    return (settings.known & guild_settings_self_quarantine) &&
           (settings.self_member & guild_self_member_quarantined);
}

struct GuildInput {
    u64 id;
    Utf8View name;
    GuildMutationKind kind;
    u64 owner_id;
    Utf8View icon_hash{};
    bool icon_known = false;
    u8 upload_features = 0;
    bool upload_features_known = false;
    u8 nsfw_level = 0;
    bool nsfw_level_known = false;
    const GuildSettings* settings = nullptr;
    // "banner" key seen: the hash (empty for null) replaces the stored one.
    Utf8View banner_hash{};
    bool banner_known = false;
};

struct ChannelInput {
    u64 id;
    u64 guild_id;
    u64 parent_id;
    Utf8View name;
    u32 position;
    u32 flags;
    u8 type;
    u64 last_message_id = 0;
    bool last_message_known = false;
    u64 member_list_id = 0;
    bool member_list_id_known = false;
    bool name_known = false;
    bool thread_archived = false;
    bool thread_archived_known = false;
    u8 content_policy = 0;
    bool content_policy_known = false;
    bool thread_locked = false;
    bool thread_locked_known = false;
    bool thread_self_joined = false;
    bool thread_self_joined_known = false;
    bool thread_listed_active = false;
    bool thread_active_known = false;
    u16 rate_limit_per_user = 0;
    bool rate_limit_known = false;
};

struct GuildRecord {
    u64 id;
    u64 owner_id;
    u64 base_permissions;
    u32 name_offset;
    u16 name_bytes;
    u16 generation;
    u32 revision;
    u32 unread_channels;
    u32 mention_count;
    u32 icon_variant;
    char icon_hash[kGuildIconHashMaximumBytes];
    u8 icon_hash_bytes;
    bool occupied : 1;
    bool unavailable : 1;
    bool permissions_resolved : 1;
    bool member_roles_known : 1;
    bool nsfw_level_known : 1;
    // User notification mute; the host projects it from guild settings.
    bool notification_muted : 1;
    // "Hide muted channels" (user guild settings hide_muted_channels): the
    // channel list leaves out muted channels and categories (the open
    // channel stays). The host projects it from guild settings.
    bool hide_muted_channels : 1;
    // Channel opt-in on a community server (user guild settings flags
    // OPT_IN_CHANNELS_ON): the channel list shows opted-in channels only.
    // The host projects it from guild settings.
    bool channel_opt_in : 1;
    u8 upload_features;
    u8 nsfw_level;
    // Guild banner hash (cdn /banners/{guild}/{hash}); a_ marks an animated one.
    char banner_hash[kGuildIconHashMaximumBytes];
    u8 banner_hash_bytes;
    GuildSettings settings;
};
static_assert(sizeof(GuildSettings) == 32 && sizeof(GuildRecord) == 160);

struct ChannelRecord {
    u64 id;
    u64 guild_id;
    u64 parent_id;
    u64 effective_permissions;
    u64 last_message_id;
    u64 last_read_message_id;
    u64 member_list_id;
    u32 name_offset;
    u32 overwrite_offset;
    u32 position;
    u32 flags;
    u32 actions;
    u32 mention_count;
    u16 name_bytes;
    u16 generation;
    u16 overwrite_count;
    bool thread_archived : 1;
    bool thread_archived_known : 1;
    bool content_policy_known : 1;
    bool thread_locked : 1;
    bool thread_locked_known : 1;
    bool thread_self_joined : 1;
    bool thread_self_joined_known : 1;
    bool thread_listed_active : 1;
    u8 content_policy;
    u32 revision;
    u8 type;
    bool occupied : 1;
    bool permissions_resolved : 1;
    bool collapsed : 1;
    bool read_state_known : 1;
    bool read_auto_ack_suppressed : 1;
    bool read_mentions_incomplete : 1;
    bool read_flags_known : 1;
    bool read_last_viewed_known : 1;
    bool thread_active_known : 1;
    bool thread_listing_synced : 1;
    // Effective notification mute (own, category or guild): not unread.
    bool notification_muted : 1;
    // Own or category mute, drawn dimmed in channel lists.
    bool notification_dimmed : 1;
    // The channel, its category or (for a thread) its parent is opted in
    // (channel override flags OPT_IN_ENABLED); read while the guild has
    // channel_opt_in.
    bool opted_in : 1;
    // Own post creation has already updated its forum/media parent read state.
    // Retained metadata updates preserve this receipt; deleting the thread clears it.
    bool thread_parent_creation_noted : 1;
    // Slowmode in slowmode_encode form; fits existing padding.
    u8 slowmode_code;
    u32 read_last_viewed;
    u32 read_flags;
};
static_assert(sizeof(ChannelRecord) == 104);
// Exact to 60 s, then 10 s / 60 s / 600 s steps (rounded down) up to 6 h.
// Every slowmode value the official client's settings offer encodes exactly.
constexpr u8 slowmode_encode(u32 seconds) noexcept {
    seconds = seconds > 21600 ? 21600 : seconds;
    return static_cast<u8>(seconds <= 60     ? seconds
                           : seconds <= 600  ? 60 + (seconds - 60) / 10
                           : seconds <= 3600 ? 114 + (seconds - 600) / 60
                                             : 164 + (seconds - 3600) / 600);
}
constexpr u32 slowmode_decode(u8 code) noexcept {
    return code <= 60    ? code
           : code <= 114 ? 60 + (code - 60u) * 10
           : code <= 164 ? 600 + (code - 114u) * 60
           : code <= 194 ? 3600 + (code - 164u) * 600
                         : 21600;
}
inline u32 channel_slowmode_seconds(const ChannelRecord& channel) noexcept {
    return slowmode_decode(channel.slowmode_code);
}
bool channel_is_thread(u8 type) noexcept;
bool channel_is_thread_parent(u8 type) noexcept;
// Thread read permission is distinct from joined/active mutation prerequisites.
// Private threads require resolved parent VIEW and known self membership or
// MANAGE_THREADS. Public reads do not depend on joining or archive state.
bool chat_state_store_thread_accessible(const ChatStateStore&, const ChannelRecord&) noexcept;
u32 chat_state_store_thread_actions(const ChatStateStore&, const ChannelRecord&) noexcept;
// Returns whether the observation was accepted, including an unchanged server-confirmed one.
// A different current self ID cannot mutate retained membership or metadata.
bool chat_state_store_set_thread_self_membership(ChatStateStore*, u64 thread_id, u64 self_user_id,
                                                    bool joined, bool known = true) noexcept;
bool chat_state_store_set_thread_archived(ChatStateStore*, u64 thread_id, u64 self_user_id,
                                             bool archived, bool known = true) noexcept;
bool chat_state_store_set_thread_locked(ChatStateStore*, u64 thread_id, u64 self_user_id, bool locked,
                                           bool known = true) noexcept;
// Active inventory is independent of stored archive metadata and read access.
bool chat_state_store_set_thread_active(ChatStateStore*, u64 thread_id, u64 self_user_id,
                                           bool active, bool known = true) noexcept;
// Only a valid complete LIST_SYNC scope establishes a parent's listing freshness.
bool chat_state_store_set_thread_listing_synced(ChatStateStore*, u64 parent_id, u64 self_user_id,
                                                   bool synced = true) noexcept;
// Zero scope IDs cover every stored thread. Drops known state on a gap,
// account/READY replacement or unavailable guild without inventing negative results.
void chat_state_store_invalidate_thread_authority(ChatStateStore*, u64 guild_id = 0,
                                                     u64 parent_id = 0) noexcept;
// Threads inherit their parent channel's age restriction without copying
// derived policy into every child on a parent update.
bool chat_state_store_channel_age_restricted(const ChatStateStore& store,
                                                const ChannelRecord& channel) noexcept;

// Enhanced (gradient and holographic) role colours mark a present secondary or
// tertiary colour with this bit, so black stays distinct from "none".
inline constexpr u32 kRoleColorPresent = 1u << 24;
inline constexpr bool role_color_present(u32 color) noexcept {
    return (color & kRoleColorPresent) != 0;
}
struct RoleRecord {
    u64 id;
    u64 guild_id;
    u64 permissions;
    u32 name_offset;
    u32 position;
    u32 color;
    u16 name_bytes;
    u8 self_assigned : 1;
    u8 hoist : 1;
    u8 managed : 1;
    u8 mentionable : 1;
    // Icon or unicode emoji present; the assets themselves come from the REST role list.
    u8 has_icon : 1;
    // Role flag bit 0: selectable in onboarding prompts.
    u8 in_prompt : 1;
    u8 reserved : 2;
    // colors.secondary_color / tertiary_color: RGB | kRoleColorPresent or zero.
    u32 secondary_color;
    u32 tertiary_color;
};

static_assert(sizeof(RoleRecord) == 48);

#pragma pack(push, 1)
struct PermissionOverwriteRecord {
    u64 id;
    u64 allow;
    u64 deny;
    u8 type;
};
#pragma pack(pop)

static_assert(sizeof(PermissionOverwriteRecord) == 25);

struct RoleInput {
    u64 id;
    u64 guild_id;
    u64 permissions;
    Utf8View name;
    u32 position = 0;
    u32 color = 0;
    bool hoist = false;
    bool managed = false;
    bool mentionable = false;
    bool has_icon = false;
    bool in_prompt = false;
    // Enhanced role colours: RGB | kRoleColorPresent, or zero when absent.
    u32 secondary_color = 0;
    u32 tertiary_color = 0;
};

struct PermissionOverwriteInput {
    u64 id;
    u64 allow;
    u64 deny;
    u8 type;
};

enum CustomEmojiFlag : u8 {
    custom_emoji_animated = 1u << 0,
    custom_emoji_available = 1u << 1,
    custom_emoji_managed = 1u << 2,
    custom_emoji_require_colons = 1u << 3,
};

struct CustomEmojiRecord {
    u64 id;
    u64 guild_id;
    u32 role_offset;
    u16 role_count;
    u8 name_bytes;
    u8 flags;
    char name[kCustomEmojiNameMaximumBytes];
};

struct CustomEmojiInput {
    u64 id;
    Utf8View name;
    ConstSlice<u8> role_ids_le;
    u16 role_count;
    u8 flags;
    u8 reserved;
};

enum class SelfMfaState : u8 {
    unknown,
    disabled,
    enabled,
};

using PermissionReserve = bool (*)(void* context, u32 required_roles,
                                          u32 required_overwrites) noexcept;

struct ChatStateStore {
    ReadStateStore* read_state;
    PrivateChannelStore* private_channels;
    GuildRecord* guilds;
    ChannelRecord* channels;
    // The live prefix is sorted by (guild_id, id); mutations may move role records.
    RoleRecord* roles;
    PermissionOverwriteRecord* overwrites;
    CustomEmojiRecord* emojis;
    u64* emoji_roles;
    u16* guild_order;
    u16* channel_order;
    u8* names;
    u32 guild_capacity;
    u32 channel_capacity;
    u32 role_capacity;
    u32 overwrite_capacity;
    PermissionReserve permission_reserve;
    void* permission_reserve_context;
    u32 role_backed_count;
    u32 overwrite_backed_count;
    u32 emoji_capacity;
    u32 emoji_role_capacity;
    u32 guild_count;
    u32 channel_count;
    u32 role_count;
    u32 overwrite_count;
    u32 emoji_count;
    u32 emoji_role_count;
    u32 name_bank_bytes;
    u32 name_used;
    u32 name_wasted;
    u32 rejected;
    u32 guild_mutations;
    u32 channel_mutations;
    u32 permission_mutations;
    u32 activity_mutations;
    u32 emoji_mutations;
    u32 compactions;
    u64 selected_guild_id;
    u64 selected_channel_id;
    u64 self_user_id;
    UploadPolicy upload_policy;
    u8 self_premium_type;
    // Uses the existing alignment gap before last_private_channel_id.
    AccountContentPolicy content_policy;
    u64 last_private_channel_id;
    u64 restore_channel_id;
    u64 restore_guild_id;
    u64 restore_deadline_ms;
    u32 selection_revision;
    u32 navigation_intent_revision;
    u8 active_name_bank;
    bool home_selected;
    SelfMfaState self_mfa;
    // Every slot below a hint is occupied, so READY insertion is amortized O(1).
    // Both fit existing tail padding.
    u16 guild_free_hint;
    u16 channel_free_hint;
};
static_assert(sizeof(ChatStateStore) == 288);

enum class ChatStateStoreError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    guild_capacity,
    channel_capacity,
    role_capacity,
    overwrite_capacity,
    emoji_capacity,
    emoji_role_capacity,
    name_capacity,
    not_found,
};

struct ChatStateStoreResult {
    u32 order_index;
    ChatStateStoreError error;
    bool inserted;
    bool selection_changed;

    constexpr bool ok() const noexcept {
        return error == ChatStateStoreError::none;
    }
};

enum class ChatStateSelectionError : u8 {
    none,
    invalid_input,
    not_found,
    unavailable,
    not_selectable,
};

struct ChatStateSelectionResult {
    ChatStateSelectionError error;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == ChatStateSelectionError::none;
    }
};

bool chat_state_store_initialize(ChatStateStore* store, Slice<GuildRecord> guilds,
                                    Slice<u16> guild_order, Slice<ChannelRecord> channels,
                                    Slice<u16> channel_order, Slice<u8> double_buffered_names) noexcept;
// For zero-filled (freshly committed) arrays: leaves their contents unchanged,
// so record pages are touched only as slots fill. Active counts bound all
// order reads.
bool chat_state_store_initialize_sparse(ChatStateStore* store, Slice<GuildRecord> guilds,
                                        Slice<u16> guild_order, Slice<ChannelRecord> channels,
                                        Slice<u16> channel_order, Slice<u8> double_buffered_names) noexcept;
bool chat_state_store_bind_permissions(ChatStateStore* store, Slice<RoleRecord> roles,
                                          Slice<PermissionOverwriteRecord> overwrites) noexcept;
// Logical slices are stable reserved ranges, initially with zero backed rows.
// Context is opaque and may be null when the callback supports it. Success
// guarantees both requested prefixes writable/zeroed, without model mutation.
bool chat_state_store_bind_permissions_reserved(ChatStateStore* store, Slice<RoleRecord> roles,
                                                   Slice<PermissionOverwriteRecord> overwrites,
                                                   PermissionReserve reserve,
                                                   void* context) noexcept;
bool chat_state_store_prepare_permissions(ChatStateStore* store, u32 required_roles,
                                             u32 required_overwrites) noexcept;
// Requires an empty emoji model and leaves both array contents unchanged.
// The caller owns both complete writable arrays for the model lifetime.
// Active counts bound all reads. Publication initializes each active prefix.
bool chat_state_store_bind_emojis_sparse(ChatStateStore* store, Slice<CustomEmojiRecord> emojis,
                                            Slice<u64> emoji_roles) noexcept;
ChatStateStoreResult chat_state_store_upsert_guild(ChatStateStore* store,
                                                         const GuildInput& input) noexcept;
bool guild_settings_valid(const GuildSettings& settings) noexcept;
// Applies only the observed fields; a two-factor requirement change re-resolves
// guild permissions.
bool chat_state_store_set_guild_settings(ChatStateStore* store, u64 guild_id,
                                            const GuildSettings& settings) noexcept;
// Unix seconds. Keeps the current-account timeout separate from roles.
bool chat_state_store_set_self_timeout(ChatStateStore* store, u64 guild_id, u64 until_seconds) noexcept;
// Read one already validated role from caller-owned update input.
// The preflight never mutates the store or keeps a callback/view.
using RoleInputRead = bool (*)(const void* context, u32 index, RoleInput* output) noexcept;
using ChannelInputRead = bool (*)(const void* context, u32 index,
                                         ChannelInput* output) noexcept;
ChatStateStoreError chat_state_store_preflight_channel_batch(const ChatStateStore& store,
                                                                   u32 channel_count,
                                                                   ChannelInputRead read_channel,
                                                                   const void* context) noexcept;
ChatStateStoreError chat_state_store_preflight_guild_role_update(const ChatStateStore& store,
                                                                       const GuildInput& guild,
                                                                       u32 role_count,
                                                                       RoleInputRead read_role,
                                                                       const void* context) noexcept;
// Inputs are already validated as unique packet-order IDs. Readers and
// their borrowed views must not change through preflight and commit.
ChatStateStoreError chat_state_store_preflight_guild_create(
    const ChatStateStore& store, const GuildInput& guild, u32 role_count,
    RoleInputRead read_role, const void* role_context, u32 channel_count,
    ChannelInputRead read_channel, const void* channel_context) noexcept;
// Checks only that the store is initialized; not a global consistency check. Paths that
// cannot compact validate header/slot/target bounds without walking other names.
ChatStateStoreError chat_state_store_preflight_channel(const ChatStateStore& store,
                                                             const ChannelInput& input) noexcept;
ChatStateStoreResult chat_state_store_upsert_channel(ChatStateStore* store,
                                                           const ChannelInput& input) noexcept;
bool chat_state_store_remove_guild(ChatStateStore* store, u64 guild_id, bool unavailable) noexcept;
bool chat_state_store_remove_channel(ChatStateStore* store, u64 channel_id) noexcept;
void chat_state_store_remove_guild_channels(ChatStateStore* store, u64 guild_id) noexcept;
void chat_state_store_clear_guilds(ChatStateStore* store) noexcept;
u32 chat_state_store_clear_private_channels(ChatStateStore* store) noexcept;
bool chat_state_store_finish_restore(ChatStateStore* store) noexcept;
bool chat_state_store_expire_restore(ChatStateStore* store, u64 now_ms) noexcept;
const GuildRecord* chat_state_store_find_guild(const ChatStateStore& store,
                                                         u64 guild_id) noexcept;
const ChannelRecord* chat_state_store_find_channel(const ChatStateStore& store,
                                                             u64 channel_id) noexcept;
const RoleRecord* chat_state_store_find_role(const ChatStateStore& store, u64 guild_id,
                                                       u64 role_id) noexcept;
Utf8View chat_state_store_guild_name(const ChatStateStore& store,
                                        const GuildRecord& record) noexcept;
Utf8View chat_state_store_guild_icon_hash(const GuildRecord& record) noexcept;
// Empty when the guild has no banner (or it is not known yet).
Utf8View chat_state_store_guild_banner_hash(const GuildRecord& record) noexcept;
Utf8View chat_state_store_channel_name(const ChatStateStore& store,
                                          const ChannelRecord& record) noexcept;
Utf8View chat_state_store_role_name(const ChatStateStore& store,
                                       const RoleRecord& record) noexcept;
bool channel_is_message_capable(u8 type) noexcept;
// Text chat of a guild voice or stage channel ("text in voice").
constexpr bool channel_is_voice_chat(u8 type) noexcept {
    return type == 2 || type == 13;
}
constexpr bool channel_is_private(u8 type) noexcept {
    return type == 1 || type == 3;
}
bool chat_state_store_touch_private_channel(ChatStateStore* store, u64 channel_id) noexcept;
ChatStateSelectionResult chat_state_store_select_home(ChatStateStore* store) noexcept;
bool chat_state_store_channel_visible(const ChatStateStore& store,
                                         const ChannelRecord& channel) noexcept;
u32 channel_actions(u8 type, u64 permissions, bool resolved) noexcept;
bool chat_state_store_channel_unread(const ChatStateStore& store,
                                        const ChannelRecord& channel) noexcept;
bool chat_state_store_self_has_role(const ChatStateStore& store, u64 guild_id, u64 role_id) noexcept;
const CustomEmojiRecord* chat_state_store_find_emoji(const ChatStateStore& store,
                                                         u64 emoji_id) noexcept;
Utf8View chat_state_store_emoji_name(const CustomEmojiRecord& record) noexcept;
bool chat_state_store_emoji_usable(const ChatStateStore& store,
                                      const CustomEmojiRecord& record) noexcept;
bool chat_state_store_can_replace_guild_emojis(const ChatStateStore& store, u64 guild_id,
                                                  u32 emoji_count, u32 role_count) noexcept;
bool chat_state_store_replace_guild_emojis(ChatStateStore* store, u64 guild_id,
                                              ConstSlice<CustomEmojiInput> emojis) noexcept;
void chat_state_store_remove_guild_emojis(ChatStateStore* store, u64 guild_id) noexcept;
// Host projection of notification mutes. A muted channel keeps its mention
// count but no longer counts as unread, in its row or its guild aggregate.
// Returns true when anything changed.
bool chat_state_store_set_channel_mute(ChatStateStore* store, u64 channel_id, bool muted,
                                          bool dimmed) noexcept;
bool chat_state_store_set_guild_mute(ChatStateStore* store, u64 guild_id, bool muted) noexcept;
// Changes a guild's "Hide muted channels" choice; true when it changed. The
// channel projection rebuilds from the bumped channel mutation counter.
bool chat_state_store_set_guild_hide_muted(ChatStateStore* store, u64 guild_id, bool hide) noexcept;
bool chat_state_store_set_guild_channel_opt_in(ChatStateStore* store, u64 guild_id, bool enabled) noexcept;
bool chat_state_store_set_channel_opted_in(ChatStateStore* store, u64 channel_id, bool opted_in) noexcept;
bool chat_state_store_note_message(ChatStateStore* store, u64 channel_id, u64 message_id,
                                      u64 author_id, bool current_member_mentioned, bool viewed,
                                      bool acknowledge_self = true, bool low_importance = false) noexcept;
// Reconciles validated history metadata without replaying CREATE activity or
// altering read cursors, mention counts, or manual auto-ACK suppression.
// An older or identical latest ID is an accepted no-op.
bool chat_state_store_note_history_latest(ChatStateStore* store, u64 channel_id,
                                             u64 latest_message_id) noexcept;
// Call only for a newly created post, after its thread record is retained.
// Advances a forum/media parent's latest ID without fabricating mentions.
// Own creation acknowledges only this post once; delayed receipts preserve
// newer unread state and an uncertain remaining mention count.
bool chat_state_store_note_thread(ChatStateStore* store, u64 parent_id,
                                     u64 thread_id, u64 owner_id) noexcept;
bool chat_state_store_can_replace_guild_permissions(const ChatStateStore& store, u64 guild_id,
                                                       u32 role_count, u32 overwrite_count) noexcept;
bool chat_state_store_can_replace_channel_overwrites(const ChatStateStore& store, u64 channel_id,
                                                        u32 overwrite_count) noexcept;
void chat_state_store_remove_guild_roles(ChatStateStore* store, u64 guild_id) noexcept;
bool chat_state_store_upsert_role(ChatStateStore* store, const RoleInput& input) noexcept;
bool chat_state_store_remove_role(ChatStateStore* store, u64 guild_id, u64 role_id) noexcept;
bool chat_state_store_set_self_roles(ChatStateStore* store, u64 guild_id, u64 owner_id,
                                        u64 self_user_id, ConstSlice<u64> role_ids) noexcept;
void chat_state_store_clear_channel_overwrites(ChatStateStore* store, u64 channel_id) noexcept;
bool chat_state_store_append_channel_overwrite(ChatStateStore* store, u64 channel_id,
                                                  const PermissionOverwriteInput& input) noexcept;
void chat_state_store_recompute_guild_permissions(ChatStateStore* store, u64 guild_id) noexcept;
bool chat_state_store_select_default(ChatStateStore* store) noexcept;
ChatStateSelectionResult chat_state_store_select_guild(ChatStateStore* store,
                                                             u64 guild_id) noexcept;
ChatStateSelectionResult chat_state_store_select_channel(ChatStateStore* store,
                                                               u64 channel_id) noexcept;

} // namespace maicord
