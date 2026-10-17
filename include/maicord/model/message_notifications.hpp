#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
enum class MessageNotificationLevel : u8 { all, mentions, none, inherit };
enum MessageNotificationField : u16 {
    notification_level = 1, notification_muted = 2, notification_mute_end = 4,
    notification_suppress_everyone = 8, notification_suppress_roles = 16,
    notification_mobile_push = 32, notification_flags = 64,
    // Guild rows only: "Hide muted channels" (hide_muted_channels).
    notification_hide_muted = 128,
};
// end_ms == 0 means indefinite/reset when the end field is present. Missing
// end field preserves the old value; missing mute_config is not null.
struct MessageNotificationSetting {
    u64 mute_end_ms;
    u32 flags;
    u16 fields;
    MessageNotificationLevel level;
    bool muted, suppress_everyone, suppress_roles, mobile_push, hide_muted_channels;
};
struct MessageNotificationGuildSetting { u64 guild_id; MessageNotificationSetting setting; bool incomplete; };
struct MessageNotificationOverride { u64 guild_id, channel_id; MessageNotificationSetting setting; };
struct MessageNotificationThreadSetting { u64 guild_id, thread_id; MessageNotificationSetting setting; };
using MessageNotificationReserve = bool(*)(void*, u32, u32, u32) noexcept;
struct MessageNotificationStore {
    Slice<MessageNotificationGuildSetting> guilds;
    Slice<MessageNotificationOverride> overrides;
    Slice<MessageNotificationThreadSetting> threads;
    MessageNotificationReserve reserve;
    void* reserve_context;
    u64 version;
    u32 guild_count, override_count, thread_count, revision;
    bool complete;
};
enum class MessageNotificationApplyError : u8 { none, input, duplicate, capacity, stale };
bool message_notifications_initialize(MessageNotificationStore*,
    Slice<MessageNotificationGuildSetting>, Slice<MessageNotificationOverride>,
    Slice<MessageNotificationThreadSetting>, MessageNotificationReserve = nullptr, void* = nullptr) noexcept;
// Source slices must be immutable and disjoint from active store memory. All
// admission/reservation completes before publication. Full entries replace
// their override set; partial READY preserves entries omitted from the packet.
MessageNotificationApplyError message_notifications_apply_settings(MessageNotificationStore*,
    ConstSlice<MessageNotificationGuildSetting>, ConstSlice<MessageNotificationOverride>,
    bool partial, u64 version) noexcept;
MessageNotificationApplyError message_notifications_upsert_thread(MessageNotificationStore*,
    const MessageNotificationThreadSetting&) noexcept;
void message_notifications_remove_thread(MessageNotificationStore*, u64 thread_id) noexcept;
void message_notifications_remove_guild(MessageNotificationStore*, u64 guild_id) noexcept;
void message_notifications_clear(MessageNotificationStore*) noexcept;
// Existing known scopes keep working when an omitted scope cannot be admitted.
// Unknown IDs mark the implicit default projection incomplete, without evicting
// a known row; callers may recover via a later full READY/settings entry.
void message_notifications_mark_incomplete(MessageNotificationStore*, u64 guild_id) noexcept;
const MessageNotificationGuildSetting* message_notifications_guild(const MessageNotificationStore&, u64) noexcept;
const MessageNotificationOverride* message_notifications_override(const MessageNotificationStore&, u64, u64) noexcept;
const MessageNotificationThreadSetting* message_notifications_thread(const MessageNotificationStore&, u64) noexcept;
bool message_notification_muted(const MessageNotificationSetting&, u64 now_ms) noexcept;
bool message_notifications_guild_muted(const MessageNotificationStore&, u64 guild_id, u64 now_ms) noexcept;
// A channel override (own or category) mute; guild 0 holds direct messages.
bool message_notifications_override_muted(const MessageNotificationStore&, u64 guild_id, u64 channel_id,
                                          u64 now_ms) noexcept;
// Earliest finite mute end still ahead of now across every scope; 0 if none.
u64 message_notifications_next_mute_expiry(const MessageNotificationStore&, u64 now_ms) noexcept;

enum class MessageNotificationStatus : u8 { unknown, online, idle, dnd, invisible, offline };
enum MessageNotificationGlobalField : u8 { notification_quiet = 1, notification_focus_expiry = 2,
    notification_status = 4, notification_status_expiry = 8 };
struct MessageNotificationGlobal {
    u64 focus_expires_ms, status_expires_ms;
    u8 fields;
    bool quiet;
    MessageNotificationStatus status;
};
void message_notifications_apply_global(MessageNotificationGlobal*, const MessageNotificationGlobal&, bool partial) noexcept;

// A set gate bit rejects. A known clear bit (known_gates) differs from an
// unknown gate. Callers need only the gates applicable to the message kind.
enum MessageNotificationGate : u32 {
    notification_gate_blocked = 1u << 0, notification_gate_ignored = 1u << 1,
    notification_gate_spammer = 1u << 2, notification_gate_managed = 1u << 3,
    notification_gate_lurking = 1u << 4, notification_gate_request = 1u << 5,
    notification_gate_own_activity = 1u << 6, notification_gate_active_application = 1u << 7,
    notification_gate_rpc_suppressed = 1u << 8, notification_gate_restricted_hours = 1u << 9,
    notification_gate_required_action = 1u << 10, notification_gate_streamer = 1u << 11,
    notification_gate_overlay = 1u << 12, notification_gate_changelog = 1u << 13,
    notification_gate_age_access = 1u << 14,
    notification_gate_interaction_relationship = 1u << 15,
};
struct MessageNotificationPolicyInput {
    const MessageNotificationStore* settings;
    u64 self_id, author_id, guild_id, channel_id, parent_id, category_id, now_ms;
    ConstSlice<u64> mention_roles, self_roles;
    MessageNotificationGlobal global;
    u32 message_flags, gates, known_gates, required_gates;
    u16 message_type;
    u8 channel_type;
    MessageNotificationLevel guild_default;
    bool ready, live, duplicate, identities_known, roles_known, type_known;
    bool guild_default_known, direct_mention, everyone_mention;
    bool focused, selected, sidebar_selected, current_voice_channel;
    bool self_mentionable_system;
    bool current_voice_channel_known;
};
enum class MessageNotificationReason : u8 { eligible, unknown, not_live, duplicate, silent,
    self, special, dnd, quiet, muted, nothing, not_opted_in, selected, not_mentioned, not_joined };
enum class MessageNotificationScopeMode : u8 { generic, message_thread };
struct MessageNotificationScope {
    MessageNotificationReason reason;
    MessageNotificationLevel level;
    bool suppress_everyone, suppress_roles;
};
// generic resolves the level channel -> immediate parent -> guild/default, with
// mute and opt-in checked on the guild, channel and immediate parent.
// message_thread also requires a joined thread: explicit thread flags set the
// level, otherwise parent -> category -> guild/default, and the category mute applies.
MessageNotificationScope message_notification_scope_resolve(const MessageNotificationPolicyInput&,
    MessageNotificationScopeMode = MessageNotificationScopeMode::message_thread) noexcept;
// Valid known UNKNOWN status projects ONLINE while connected. Missing fields
// remain unknown; finite status/focus expiries only transform known fields.
MessageNotificationGlobal message_notification_effective_global(const MessageNotificationGlobal&,u64 now_ms,
    bool connected) noexcept;
// Admission is independent of desktop/sound device output booleans. Callers
// choose their exact required gate mask; this overload uses the input's required_gates.
MessageNotificationReason message_notification_admit(const MessageNotificationPolicyInput&) noexcept;
// Override the gate mask without copying the input's roles/global/scope facts.
MessageNotificationReason message_notification_admit(const MessageNotificationPolicyInput&,u32 required_gates) noexcept;
enum class MessageReadBadge : u8 { none, mention, low_importance };
// Read-state automations use relationship, mention and mute facts only. Device
// output, focus, quiet mode, silent messages and popup admission do not apply.
MessageReadBadge message_read_badge(const MessageNotificationPolicyInput&,
                                    bool mention_on_all_messages) noexcept;
} // namespace maicord
