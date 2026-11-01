#pragma once

#include "maicord/core/slice.hpp"

// What the application calls to ask plugins about decorations and buttons, and to report clicks. The
// runtime (decorations.cpp, buttons.cpp) installs g_plugin_surfaces when the first plugin that decorates
// or declares a button is enabled and removes it with the last, so the application pays one null test per
// pass until then. This header is free of the plugin ABI header and of the UI headers.
//
// The pull model: decorate hooks run in the application's prepare pass (never while laying out or
// painting), each result is cached per plugin by the entity's revision, and the UI reads the merged
// cached verdicts through the queries in maicord/ui/plugins/{render,slots}.hpp, which this runtime
// installs in g_ui_plugin_render_query and g_ui_plugin_slot_query. Nothing the UI calls runs plugin code.
namespace maicord {

// The numbers the inputs' flag and enum fields carry are the plugin ABI's (plugin_abi.h; reads.cpp checks them), so
// application code names them without including the ABI header.
inline constexpr u32 kPluginMessageOwn = 1, kPluginMessageBot = 2, kPluginMessageEdited = 4, kPluginMessagePinned = 8,
                     kPluginMessageReply = 16, kPluginMessageMedia = 32, kPluginMessageSystem = 64,
                     kPluginMessageDeleted = 128, kPluginMessageLocal = 256, kPluginMessagePrivate = 512,
                     kPluginMessageNsfw = 1024;
inline constexpr u32 kPluginRelationshipNone = 0, kPluginRelationshipFriend = 1, kPluginRelationshipBlocked = 2,
                     kPluginRelationshipIncoming = 3, kPluginRelationshipOutgoing = 4;
inline constexpr u32 kPluginUserMemberRow = 1, kPluginUserMessageAuthor = 2, kPluginUserProfile = 3;
inline constexpr u32 kPluginNeedContent = 1, kPluginNeedAuthor = 2;
inline constexpr u32 kPluginUserBot = 1, kPluginUserSelf = 2;
inline constexpr u32 kPluginPresenceUnknown = 0, kPluginPresenceOnline = 1, kPluginPresenceIdle = 2,
                     kPluginPresenceDnd = 3, kPluginPresenceOffline = 4;
inline constexpr u32 kPluginPlatformDesktop = 1, kPluginPlatformMobile = 2, kPluginPlatformWeb = 4;
inline constexpr u32 kPluginChannelNsfw = 1, kPluginChannelUnread = 2, kPluginChannelMuted = 4,
                     kPluginChannelPrivate = 8, kPluginChannelCanSend = 16;
inline constexpr u32 kPluginGuildUnread = 1, kPluginGuildMuted = 2;
inline constexpr u32 kPluginHeaderChannel = 1, kPluginHeaderMemberList = 2;
// The entities the application decorates. Fields are what the matching mc_* struct of the ABI carries.
struct PluginMessageInput {
    u64 channel_id, guild_id, message_id, author_id, reply_to, timestamp_ms, edited_ms;
    u32 flags;     // MC_MESSAGE_*
    u32 type;      // Discord message type
    u32 revision;  // the message store's revision of this message
    u32 relationship, attachment_count, embed_count;
    // Only what the installed hooks asked for (PluginSurfaceHost::message_needs); empty otherwise.
    Utf8View content, author_name;
};
struct PluginUserInput {
    u64 user_id, guild_id, channel_id;
    // MESSAGE_AUTHOR: the message the name is on, so that message's decoration carries the user's name prefix
    // and suffix; 0 otherwise.
    u64 message_id;
    u32 where;      // MC_USER_MEMBER_ROW, MC_USER_MESSAGE_AUTHOR or MC_USER_PROFILE
    u32 flags;      // MC_USER_*
    u32 relationship, presence, platforms;
    Utf8View name, username;
};
struct PluginChannelInput {
    u64 channel_id, guild_id, parent_id, self_permissions;
    u32 type, flags, mention_count; // flags: MC_CHANNEL_*
    Utf8View name;
};
struct PluginGuildInput {
    u64 guild_id, owner_id, self_permissions;
    u32 flags, mention_count, member_count, online_count; // flags: MC_GUILD_*
    Utf8View name;
};
struct PluginHeaderInput {
    u64 channel_id, guild_id;
    u32 where; // MC_HEADER_CHANNEL or MC_HEADER_MEMBER_LIST
    u32 member_count, online_count;
    Utf8View name;
};

// One prepare pass: all decorate and visibility calls share the aggregate budget (design 10: 4 ms), and the
// rest wait for the next pass. Zero-initialize, then set nothing: begin fills the budget.
inline constexpr u64 kPluginDecoratePassBudgetUs = 4'000;
struct PluginDecoratePass {
    u64 budget_us, spent_us;
    u32 calls;
    // A call was skipped for budget, so a later pass has more to do.
    bool exhausted;
};

// The UI surface a click came from (the kUiPluginSlot* numbers of ui/plugins/slots.hpp) and what it targets.
struct PluginSurfaceTarget {
    u32 surface;
    u64 channel_id, message_id, user_id, guild_id;
};

// A plugin button that a message's hover bar has no room for: the host lists these in the message menu's
// "Plugins" submenu. owner_id and action_id are the ids plugin_surfaces_activate takes.
struct PluginButtonRef {
    u64 owner_id, action_id;
    Utf8View label;
    bool danger;
};

struct PluginSurfaceHost {
    // Bit (1 << kind) of the decorate hook kinds some enabled plugin subscribes to: message 0, user 1, channel 2,
    // guild 3, header 4. Callers skip building inputs for a clear bit.
    u32 wants;
    // The union of MC_NEED_* over the enabled DECORATE_MESSAGE hooks: which message fields to fill.
    u32 message_needs;
    // Each returns true when every plugin's verdict for the entity is current, false when a call waited for budget.
    bool (*message)(PluginDecoratePass*, const PluginMessageInput&) noexcept;
    bool (*user)(PluginDecoratePass*, const PluginUserInput&) noexcept;
    bool (*channel)(PluginDecoratePass*, const PluginChannelInput&) noexcept;
    bool (*guild)(PluginDecoratePass*, const PluginGuildInput&) noexcept;
    bool (*header)(PluginDecoratePass*, const PluginHeaderInput&) noexcept;
    // Runs the message hover bar's `visible` predicates for one message (cached per message revision).
    bool (*popover)(PluginDecoratePass*, const PluginMessageInput&) noexcept;
    // Runs the click of a plugin control: a descriptor button (action_id from the slot node) or a decoration
    // button or card button (its tag). Gesture context. `message` is the clicked or hovered message for the
    // hover bar and message accessories (with its content), else null. False when nothing handled it.
    bool (*activate)(u64 owner_id, const PluginSurfaceTarget&, u64 action_id, const PluginMessageInput* message) noexcept;
    // The message's visible popover buttons past the first `bar` ones, in plugin order.
    u32 (*overflow)(u64 message_id, u32 bar, Slice<PluginButtonRef> output) noexcept;
    // Marks every cached pull verdict stale (an application-side input changed, e.g. the account switched).
    void (*refresh)() noexcept;
    // Runs the `visible` predicates of the buttons that don't depend on a message, with the open channel
    // (channel switch, enable).
    void (*refresh_buttons)(u64 channel_id, u64 guild_id) noexcept;
    // Session reveal of a collapsed message ("Show"): the cached verdict stays and the query reports the
    // message shown. Bounded; the oldest reveal is forgotten first. True when it was newly revealed.
    bool (*reveal)(u64 message_id) noexcept;
};
inline constinit const PluginSurfaceHost* g_plugin_surfaces = nullptr;

// What mc_api.read_user, read_channel, read_guild and read_permissions read: MaiCord's cache, never the network.
// The application installs it (the tests install a fake); each function fills one of the inputs above and
// returns false when the entity isn't cached. Views stay valid until the application's stores next change,
// which is after the plugin call that read them returns.
struct PluginReader {
    // names come from guild_id when it isn't 0.
    bool (*user)(u64 user_id, u64 guild_id, PluginUserInput* out) noexcept;
    bool (*channel)(u64 channel_id, PluginChannelInput* out) noexcept;
    bool (*guild)(u64 guild_id, PluginGuildInput* out) noexcept;
    // The permission bits user_id holds in guild_id (channel_id 0: server-wide, else in that channel).
    bool (*permissions)(u64 guild_id, u64 channel_id, u64 user_id, u64* bits) noexcept;
};
inline constinit const PluginReader* g_plugin_reader = nullptr;

// Changes whenever any decoration or button state the UI reads changed; the application compares it with the
// value it last rendered and rebuilds retained text. Message decorations also advance
// g_ui_plugin_render_revision.
inline u64 g_plugin_surface_revision = 0;
// What the message verdicts contribute to g_ui_plugin_render_revision (mixed with the application's own
// reveal state): changes with every message decoration change.
inline u64 g_plugin_message_revision = 0;

inline bool plugin_surfaces_wants(u32 kind) noexcept {
    return g_plugin_surfaces && (g_plugin_surfaces->wants & (1u << kind));
}

} // namespace maicord
