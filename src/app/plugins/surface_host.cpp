// The application's side of plugin decorations and buttons: the bounded prepare pass that gives the scenes'
// entities to the installed plugin host, polling for changes, clicks, and the cache reader plugins read through.
// The plugin code itself runs in the runtime (decorations.cpp, buttons.cpp); nothing here runs it on a paint path.
#include "surface_host.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/guild/detail_store.hpp"
#include "maicord/model/guild/permission_compute.hpp"
#include "maicord/model/user/private_channel_store.hpp"
#include "maicord/model/user/relationship_store.hpp"
#include "maicord/protocol/gateway/client_status.hpp"
#include "maicord/ui/plugins/render.hpp"
#include "maicord/ui/plugins/slot_surfaces.hpp"
#include "maicord/ui/chat/private_channel_title.hpp"
#include "../account/account_presentation.hpp"
#include "../account/profile.hpp"
#include "../account/account_ui_runtime.hpp"
#include "../account/friends.hpp"
#include "plugins.hpp"
#include "../shell/content_input.hpp"
#include "../shell/state.hpp"

namespace maicord::app_detail {
namespace {

// Messages considered per pass (laid-out rows first, then the resident window newest first).
constexpr u32 kScanLimit = 256;

struct HostState {
    u64 prepared_signature;  // inputs of the last pass that left nothing to evaluate
    u64 rendered_revision;   // g_plugin_surface_revision the text was last built for
    u64 context_channel;     // the channel the button predicates were last run for
    bool pending;            // a pass ran out of budget; more waits
    // The message whose popover buttons were last prepared, at its revision and the plugin state's revision.
    u64 hover_message, hover_surface_revision;
    u32 hover_revision;
};
constinit HostState g_host{};

// ---- inputs -------------------------------------------------------------------------------------

u32 relationship_of(u64 user_id) noexcept {
    const auto* store = application_friends_relationships();
    const auto* record = store && user_id ? relationship_store_find(*store, user_id) : nullptr;
    if (!record) return kPluginRelationshipNone;
    switch (record->type) {
    case RelationshipType::friend_: return kPluginRelationshipFriend;
    case RelationshipType::blocked: return kPluginRelationshipBlocked;
    case RelationshipType::incoming: return kPluginRelationshipIncoming;
    case RelationshipType::outgoing: return kPluginRelationshipOutgoing;
    default: return kPluginRelationshipNone;
    }
}
u32 presence_of(u8 packed) noexcept {
    switch (client_status_overall(packed)) {
    case ClientPlatformStatus::online: return kPluginPresenceOnline;
    case ClientPlatformStatus::idle: return kPluginPresenceIdle;
    case ClientPlatformStatus::dnd: return kPluginPresenceDnd;
    default: return kPluginPresenceOffline;
    }
}
u32 platforms_of(u8 packed) noexcept {
    u32 platforms = 0;
    if (client_status_get(packed, ClientPlatform::desktop) != ClientPlatformStatus::offline)
        platforms |= kPluginPlatformDesktop;
    if (client_status_get(packed, ClientPlatform::mobile) != ClientPlatformStatus::offline)
        platforms |= kPluginPlatformMobile;
    if (client_status_get(packed, ClientPlatform::web) != ClientPlatformStatus::offline)
        platforms |= kPluginPlatformWeb;
    return platforms;
}
u64 guild_of_channel(const AccountUiRuntime& ui, u64 channel_id) noexcept {
    const auto* channel = channel_id ? chat_state_store_find_channel(ui.chat_state, channel_id) : nullptr;
    return channel ? channel->guild_id : 0;
}
bool user_is_bot(const AccountUiRuntime& ui, u64 user_id) noexcept {
    const auto* appearance = user_id ? user_appearance_find(ui.user_store.appearance, user_id) : nullptr;
    return appearance && (appearance->fields & appearance_bot) && appearance->bot;
}

// The message as the host describes it. Content and the author's name only when some hook asked for them.
PluginMessageInput message_input(const AccountUiRuntime& ui, const MessageStore& store, MessageHandle handle,
                                 const MessageRecord& record, u32 needs) noexcept {
    PluginMessageInput in{};
    in.channel_id = record.channel_id;
    in.message_id = record.id;
    in.author_id = record.author_id;
    in.timestamp_ms = record.timestamp_ms;
    in.edited_ms = record.edited_timestamp_ms;
    in.type = record.type;
    in.revision = record.revision;
    const auto* channel = chat_state_store_find_channel(ui.chat_state, record.channel_id);
    in.guild_id = channel ? channel->guild_id : 0;
    const auto* rich = message_rich_store_find(ui.message_rich_store, record.id);
    if (rich && rich->message_revision != record.revision) rich = nullptr;
    u32 flags = 0;
    if (record.author_id && record.author_id == ui.chat_state.self_user_id) flags |= kPluginMessageOwn;
    if (user_is_bot(ui, record.author_id)) flags |= kPluginMessageBot;
    if (record.edited_timestamp_ms) flags |= kPluginMessageEdited;
    if (record.presentation_flags & message_presentation_pinned) flags |= kPluginMessagePinned;
    if (rich && rich->reference.type != 1 && rich->reference.state != GatewayMessageReferenceState::none) {
        flags |= kPluginMessageReply;
        in.reply_to = rich->reference.message_id;
    }
    if (rich && (rich->attachment_count || rich->embed_count)) flags |= kPluginMessageMedia;
    if (record.delivery != MessageDeliveryState::authoritative) flags |= kPluginMessageLocal;
    if (channel && channel_is_private(channel->type)) flags |= kPluginMessagePrivate;
    if (channel && chat_state_store_channel_age_restricted(ui.chat_state, *channel)) flags |= kPluginMessageNsfw;
    in.flags = flags;
    in.relationship = relationship_of(record.author_id);
    in.attachment_count = rich ? rich->attachment_count : 0;
    in.embed_count = rich ? rich->embed_count : 0;
    if ((needs & kPluginNeedContent) && record.content_availability == MessageContentAvailability::available)
        in.content = message_store_content(store, handle);
    if (needs & kPluginNeedAuthor)
        if (const auto* user = user_store_find(ui.user_store, record.author_id))
            in.author_name = user_store_name(ui.user_store, *user);
    return in;
}

// A user as shown in a row or on a message; the guild nickname when the member is resident.
bool user_input(const AccountUiRuntime& ui, u64 user_id, u64 guild_id, u32 where, PluginUserInput* out) noexcept {
    const auto* user = user_id ? user_store_find(ui.user_store, user_id) : nullptr;
    if (!user) return false;
    PluginUserInput in{};
    in.user_id = user_id;
    in.guild_id = guild_id;
    in.where = where;
    in.flags = (user_is_bot(ui, user_id) ? kPluginUserBot : 0) |
               (user_id == ui.chat_state.self_user_id ? kPluginUserSelf : 0);
    in.relationship = relationship_of(user_id);
    const u8 packed = user_store_client_status(ui.user_store, user_id);
    in.presence = packed ? presence_of(packed) : kPluginPresenceUnknown;
    in.platforms = platforms_of(packed);
    in.name = user_store_name(ui.user_store, *user);
    if (const auto* member = guild_id ? guild_member_store_find(ui.guild_member_store, guild_id, user_id) : nullptr)
        if (const auto nick = guild_member_store_nickname(*member); nick.bytes) in.name = nick;
    in.username = user_store_username(ui.user_store, *user);
    *out = in;
    return true;
}

// A DM is named after its other member, an unnamed group DM after its members (written into storage).
bool channel_input(const AccountUiRuntime& ui, const ChannelRecord& channel, PluginChannelInput* out,
                   Slice<char> storage) noexcept {
    PluginChannelInput in{};
    in.channel_id = channel.id;
    in.guild_id = channel.guild_id;
    in.parent_id = channel.parent_id;
    in.self_permissions = channel.permissions_resolved ? channel.effective_permissions : 0;
    in.type = channel.type;
    in.mention_count = channel.mention_count;
    if (chat_state_store_channel_age_restricted(ui.chat_state, channel)) in.flags |= kPluginChannelNsfw;
    if (channel.actions & channel_action_send_messages) in.flags |= kPluginChannelCanSend;
    if (channel_is_private(channel.type)) {
        in.flags |= kPluginChannelPrivate;
        in.name = private_channel_title(ui.chat_state, &ui.user_store, channel, storage);
    } else {
        in.name = chat_state_store_channel_name(ui.chat_state, channel);
    }
    *out = in;
    return true;
}

bool guild_input(const AccountUiRuntime& ui, const GuildRecord& guild, PluginGuildInput* out) noexcept {
    PluginGuildInput in{};
    in.guild_id = guild.id;
    in.owner_id = guild.owner_id;
    in.self_permissions = guild.base_permissions;
    in.mention_count = guild.mention_count;
    in.flags = (guild.unread_channels ? kPluginGuildUnread : 0) | (guild.notification_muted ? kPluginGuildMuted : 0);
    bool counts = false;
    for (const auto& list : ui.guild_member_store.lists) {
        if (list.guild_id != guild.id || !list.member_count_known) continue;
        if (!counts || list.member_count > in.member_count) {
            in.member_count = list.member_count;
            in.online_count = list.online_count_known ? list.online_count : 0;
        }
        counts = true;
    }
    in.name = chat_state_store_guild_name(ui.chat_state, guild);
    *out = in;
    return true;
}

// ---- the prepare pass -----------------------------------------------------------------------------

u64 signature(const AccountUiRuntime& ui, const MessageStore& store) noexcept {
    u64 hash = hash64_mix(kHash64Seed, g_plugin_surface_revision);
    hash = hash64_mix(hash, store.created);
    hash = hash64_mix(hash, store.updated);
    hash = hash64_mix(hash, store.deleted);
    hash = hash64_mix(hash, store.count);
    if (store.count) {
        hash = hash64_mix(hash, store.records[store.order[0]].id);
        hash = hash64_mix(hash, store.records[store.order[store.count - 1]].id);
    }
    hash = hash64_mix(hash, ui.member_list.revision);
    hash = hash64_mix(hash, ui.user_store.revision);
    hash = hash64_mix(hash, ui.navigation_view.revision);
    hash = hash64_mix(hash, ui.active_channel_id);
    hash = hash64_mix(hash, g_app.message_scene.count);
    if (const ApplicationProfile* profile = g_app.profile; profile && profile->scene.active)
        hash = hash64_mix(hash64_mix(hash, profile->data.user_id), profile->scene.opened_ms);
    return hash;
}

bool prepare_messages(const PluginSurfaceHost& host, const AccountUiRuntime& ui, PluginDecoratePass* pass) noexcept {
    const MessageStore& store = g_app.message_view.store;
    const bool messages = host.wants & (1u << 0), users = host.wants & (1u << 1);
    if (!messages && !users) return true;
    u32 considered = 0;
    bool complete = true;
    const auto visit = [&](MessageHandle handle) noexcept {
        const MessageRecord* record = message_store_resolve(store, handle);
        if (!record || !record->id) return true;
        if (++considered > kScanLimit) return false;
        if (messages && !host.message(pass, message_input(ui, store, handle, *record, host.message_needs))) {
            complete = false;
            return false;
        }
        if (users) {
            PluginUserInput author;
            const u64 guild = guild_of_channel(ui, record->channel_id);
            if (user_input(ui, record->author_id, guild, kPluginUserMessageAuthor, &author)) {
                author.channel_id = record->channel_id;
                author.message_id = record->id;
                if (!host.user(pass, author)) {
                    complete = false;
                    return false;
                }
            }
        }
        return true;
    };
    // Laid-out rows first, then the rest of the resident window, newest first.
    bool going = true;
    const auto& scene = g_app.message_scene;
    for (u32 i = 0; going && scene.active && i < scene.count; ++i) going = visit(scene.rows[i].message);
    for (u32 i = store.count; going && i-- > 0;) going = visit(message_store_ordered_handle(store, i));
    return complete;
}

bool prepare_rows(const PluginSurfaceHost& host, const AccountUiRuntime& ui, PluginDecoratePass* pass) noexcept {
    bool complete = true;
    if (host.wants & (1u << 1)) {
        const auto& scene = g_app.member_list_scene;
        const u64 guild = guild_of_channel(ui, ui.active_channel_id);
        for (u32 i = 0; complete && scene.active && i < scene.count; ++i) {
            const auto& row = scene.rows[i];
            PluginUserInput in;
            if (!row.resident || row.kind != MemberListRowKind::member ||
                !user_input(ui, row.user_id, guild, kPluginUserMemberRow, &in))
                continue;
            in.channel_id = ui.active_channel_id;
            complete = host.user(pass, in);
        }
        // The open profile; its opening time is part of the user's revision, so every opening asks again.
        const ApplicationProfile* profile = g_app.profile;
        PluginUserInput in;
        if (complete && profile && profile->scene.active &&
            user_input(ui, profile->data.user_id, profile->data.guild_id, kPluginUserProfile, &in)) {
            in.message_id = profile->scene.opened_ms;
            complete = host.user(pass, in);
        }
    }
    if (complete && (host.wants & (1u << 2))) {
        const auto& scene = g_app.shell_navigation_scene;
        char name[256];
        for (u32 i = 0; complete && scene.active && i < scene.count; ++i) {
            const auto* channel = chat_state_store_find_channel(ui.chat_state, scene.rows[i].view.id);
            PluginChannelInput in;
            if (!channel || !channel_input(ui, *channel, &in, {name, sizeof(name)})) continue;
            const ChannelViewRow& view = scene.rows[i].view;
            if (view.unread) in.flags |= kPluginChannelUnread;
            if (view.muted) in.flags |= kPluginChannelMuted;
            in.mention_count = view.mention_count;
            complete = host.channel(pass, in);
        }
    }
    if (complete && (host.wants & (1u << 3))) {
        const auto& scene = g_app.guild_rail_scene;
        for (u32 i = 0; complete && scene.active && i < scene.count; ++i) {
            const GuildViewRow& view = scene.rows[i].view;
            if (view.folder_role == GuildFolderRole::header) continue;
            const auto* guild = chat_state_store_find_guild(ui.chat_state, view.id);
            PluginGuildInput in;
            if (!guild || !guild_input(ui, *guild, &in)) continue;
            if (view.unread) in.flags |= kPluginGuildUnread;
            if (view.muted) in.flags |= kPluginGuildMuted;
            in.mention_count = view.mention_count;
            complete = host.guild(pass, in);
        }
    }
    if (complete && (host.wants & (1u << 4)) && ui.active_channel_id) {
        const auto* channel = chat_state_store_find_channel(ui.chat_state, ui.active_channel_id);
        if (channel) {
            PluginHeaderInput in{};
            in.channel_id = channel->id;
            in.guild_id = channel->guild_id;
            in.name = chat_state_store_channel_name(ui.chat_state, *channel);
            in.where = kPluginHeaderChannel;
            complete = host.header(pass, in);
            if (complete && channel->guild_id) {
                PluginGuildInput counts;
                if (const auto* guild = chat_state_store_find_guild(ui.chat_state, channel->guild_id);
                    guild && guild_input(ui, *guild, &counts)) {
                    in.where = kPluginHeaderMemberList;
                    in.member_count = counts.member_count;
                    in.online_count = counts.online_count;
                    complete = host.header(pass, in);
                }
            }
        }
    }
    return complete;
}

// The message a click or hover refers to, as the plugin host wants it (with its content).
bool message_for(u64 message_id, u32 needs, PluginMessageInput* out) noexcept {
    if (!g_app.account_ui || !message_id) return false;
    const MessageStore& store = g_app.message_view.store;
    const MessageStoreResult found = message_store_find(store, message_id);
    const MessageRecord* record = found.ok() ? message_store_resolve(store, found.handle) : nullptr;
    if (!record) return false;
    *out = message_input(*g_app.account_ui, store, found.handle, *record, needs);
    return true;
}

// ---- the cache reader -------------------------------------------------------------------------

bool reader_user(u64 user_id, u64 guild_id, PluginUserInput* out) noexcept {
    return g_app.account_ui && user_input(*g_app.account_ui, user_id, guild_id, 0, out);
}
// A generated DM name lives here until the next read.
char g_reader_name[256];
bool reader_channel(u64 channel_id, PluginChannelInput* out) noexcept {
    const auto* ui = g_app.account_ui;
    const auto* channel = ui ? chat_state_store_find_channel(ui->chat_state, channel_id) : nullptr;
    if (!channel) return false;
    channel_input(*ui, *channel, out, {g_reader_name, sizeof(g_reader_name)});
    // The scene row, when it is laid out, knows what the cache alone doesn't.
    const auto& scene = g_app.shell_navigation_scene;
    for (u32 i = 0; scene.active && i < scene.count; ++i) {
        if (scene.rows[i].view.id != channel_id) continue;
        if (scene.rows[i].view.unread) out->flags |= kPluginChannelUnread;
        if (scene.rows[i].view.muted) out->flags |= kPluginChannelMuted;
        break;
    }
    return true;
}
bool reader_guild(u64 guild_id, PluginGuildInput* out) noexcept {
    const auto* ui = g_app.account_ui;
    const auto* guild = ui ? chat_state_store_find_guild(ui->chat_state, guild_id) : nullptr;
    if (!guild || !guild_input(*ui, *guild, out)) return false;
    // What a server overview shows; counted on request, never in a decorate pass.
    const ChatStateStore& state = ui->chat_state;
    out->verification_level = guild->settings.verification_level;
    if (const auto* details = guild_detail_store_find(ui->guild_details, guild_id)) {
        out->boost_count = details->premium_subscription_count;
        out->boost_tier = details->premium_tier;
    }
    for (u32 i = 0; i < state.channel_count; ++i) {
        const ChannelRecord& channel = state.channels[state.channel_order[i]];
        out->channel_count += channel.guild_id == guild_id && !channel_is_thread(channel.type) &&
                              chat_state_store_channel_visible(state, channel);
    }
    for (u32 i = 0; i < state.role_count; ++i) out->role_count += state.roles[i].guild_id == guild_id;
    for (u32 i = 0; i < state.emoji_count; ++i) out->emoji_count += state.emojis[i].guild_id == guild_id;
    return true;
}
// What a user can do in a server (or one of its channels): computed from the cached roles and overwrites.
bool reader_permissions(u64 guild_id, u64 channel_id, u64 user_id, u64* bits) noexcept {
    const auto* ui = g_app.account_ui;
    if (!ui || !guild_id || !user_id) return false;
    const auto* guild = chat_state_store_find_guild(ui->chat_state, guild_id);
    if (!guild) return false;
    if (user_id == ui->chat_state.self_user_id) {
        if (!channel_id) {
            *bits = guild->base_permissions;
            return true;
        }
        const auto* channel = chat_state_store_find_channel(ui->chat_state, channel_id);
        if (!channel || channel->guild_id != guild_id || !channel->permissions_resolved) return false;
        *bits = channel->effective_permissions;
        return true;
    }
    const auto* member = guild_member_store_find(ui->guild_member_store, guild_id, user_id);
    if (!member || !member->roles_known || !member->role_ids_complete) return false;
    if (member->role_count > kGuildMemberRoleIdCapacity) return false;
    PermissionMember who{};
    who.user_id = user_id;
    who.role_ids = {member->role_ids, member->role_count};
    who.communication_disabled_until = member->communication_disabled_known ? member->communication_disabled_until : 0;
    *bits = chat_state_store_member_permissions(ui->chat_state, guild_id, channel_id, who,
                                                {clock_unix_milliseconds() / 1000u, false});
    return true;
}
bool reader_voice_channel(u64 user_id, u64 guild_id, u64* channel_id) noexcept {
    const auto* ui = g_app.account_ui;
    if (!ui) return false;
    const VoiceStateStore& voice = ui->voice_states;
    // Guild 0 holds private calls; with no guild asked for, any server comes next.
    const VoiceState* state = voice_state_store_find(voice, guild_id, user_id);
    for (u32 i = 0; !state && !guild_id && i < voice.count; ++i)
        if (voice.records[i].user_id == user_id && voice.records[i].channel_id) state = &voice.records[i];
    *channel_id = state ? state->channel_id : 0;
    return state != nullptr;
}
u64 group_dm_recency(const ChatStateStore& state, u64 channel_id) noexcept {
    const auto* channel = chat_state_store_find_channel(state, channel_id);
    return private_channel_recency(channel_id, channel ? channel->last_message_id : 0);
}
// Group DMs whose members include user_id, most recently active first (an insertion sort into output).
u32 reader_shared_group_dms(u64 user_id, Slice<u64> output) noexcept {
    const auto* ui = g_app.account_ui;
    const PrivateChannelStore* store = ui ? ui->chat_state.private_channels : nullptr;
    if (!store || user_id == ui->chat_state.self_user_id) return 0;
    u32 total = 0, written = 0;
    for (u32 i = 0; i < store->count; ++i) {
        const PrivateChannelRecord& record = store->records[i];
        const auto* channel = chat_state_store_find_channel(ui->chat_state, record.id);
        if (!channel || channel->type != 3) continue; // group DMs only
        const ConstSlice<u64> recipients = private_channel_store_recipients(*store, record);
        bool member = false;
        for (u32 j = 0; j < recipients.count && !member; ++j) member = recipients.data[j] == user_id;
        if (!member) continue;
        ++total;
        const u64 recency = private_channel_recency(record.id, channel->last_message_id);
        u32 at = written;
        while (at > 0 && group_dm_recency(ui->chat_state, output[at - 1]) < recency) --at;
        if (at == output.count) continue;
        for (u32 j = minimum(written, output.count - 1); j > at; --j) output[j] = output[j - 1];
        output[at] = record.id;
        written = minimum(written + 1, output.count);
    }
    return total;
}

constexpr PluginReader kReader{reader_user,          reader_channel,         reader_guild, reader_permissions,
                               reader_voice_channel, reader_shared_group_dms};

} // namespace

void application_plugin_surfaces_prepare() noexcept {
    // The scene reads cached verdicts through the installed query; with none installed every row skips the lookup.
    g_app.message_scene.plugin_decorations = true;
    g_plugin_reader = &kReader;
    const PluginSurfaceHost* host = g_plugin_surfaces;
    if (!host || !g_app.account_ui || g_app.login.visible) {
        g_host.pending = false;
        return;
    }
    const AccountUiRuntime& ui = *g_app.account_ui;
    const MessageStore& store = g_app.message_view.store;
    if (!g_host.pending && g_host.prepared_signature == signature(ui, store)) return;
    PluginDecoratePass pass{};
    bool complete = prepare_messages(*host, ui, &pass);
    if (complete && g_plugin_surfaces == host) complete = prepare_rows(*host, ui, &pass);
    // A plugin that left during the pass invalidated the host; nothing is pending then.
    g_host.pending = g_plugin_surfaces == host && !complete;
    g_host.prepared_signature = g_plugin_surfaces ? signature(ui, store) : 0;
    if (g_host.pending) application_request_frame();
}

bool application_plugin_surfaces_stale() noexcept {
    // A plugin that just left changed the revision too: its decorations leave the text on the next build.
    return (g_plugin_surfaces && g_host.pending) || g_host.rendered_revision != g_plugin_surface_revision;
}

void application_plugin_surfaces_rendered() noexcept {
    g_host.rendered_revision = g_plugin_surface_revision;
}

bool application_plugin_surfaces_poll() noexcept {
    if (!g_app.account_ui) return false;
    // The open channel decides the buttons that depend on it.
    if (g_plugin_surfaces && g_host.context_channel != g_app.account_ui->active_channel_id) {
        application_plugin_surfaces_context_changed();
    }
    const bool changed = g_plugin_surfaces
                             ? g_host.pending || g_host.rendered_revision != g_plugin_surface_revision ||
                                   g_host.prepared_signature != signature(*g_app.account_ui, g_app.message_view.store)
                             : g_host.rendered_revision != g_plugin_surface_revision;
    if (!changed) return false;
    // Buttons and badges paint from the slot query: the retained frame and the hover bar rebuild too.
    if (g_host.rendered_revision != g_plugin_surface_revision) application_plugins_surfaces_changed();
    if (refresh_account_text(false)) application_request_frame();
    return true;
}

void application_plugin_surfaces_hover(u64 message_id) noexcept {
    const PluginSurfaceHost* host = g_plugin_surfaces;
    const MessageRecord* record = host && g_app.account_ui ? application_message_record(message_id) : nullptr;
    if (!record) return;
    if (g_host.hover_message == message_id && g_host.hover_revision == record->revision &&
        g_host.hover_surface_revision == g_plugin_surface_revision)
        return;
    PluginMessageInput in;
    if (!message_for(message_id, kPluginNeedContent | kPluginNeedAuthor, &in)) return;
    PluginDecoratePass pass{};
    // A call that waited for budget is retried by the next pointer move: don't remember it.
    if (!host->popover(&pass, in)) return;
    g_host.hover_message = message_id;
    g_host.hover_revision = record->revision;
    g_host.hover_surface_revision = g_plugin_surface_revision;
}

bool application_plugin_surfaces_activate(u64 owner_id, const PluginSurfaceTarget& target, u64 action_id) noexcept {
    const PluginSurfaceHost* host = g_plugin_surfaces;
    if (!host || !action_id) return false;
    PluginSurfaceTarget resolved = target;
    PluginMessageInput message;
    const bool has_message = message_for(target.message_id, kPluginNeedContent | kPluginNeedAuthor, &message);
    // A message's badges belong to its author.
    if (has_message && !resolved.user_id) resolved.user_id = message.author_id;
    const bool handled = host->activate(owner_id, resolved, action_id, has_message ? &message : nullptr);
    application_request_frame();
    return handled;
}

bool application_plugin_surfaces_text_click(const MessageTextHit& hit) noexcept {
    if (!hit.valid || hit.kind != MessageMarkdownSemanticKind::link ||
        !ui_plugin_render_source(static_cast<u16>(hit.source_offset >> 16u)) || !hit.message_id)
        return false;
    const PluginSurfaceHost* host = g_plugin_surfaces;
    // The plugin that wrote the link is gone: it means nothing now.
    if (!host) return true;
    const MessageStore& store = g_app.message_view.store;
    const MessageStoreResult found = message_store_find(store, hit.message_id);
    const MessageRecord* record = found.ok() ? message_store_resolve(store, found.handle) : nullptr;
    if (!record || record->revision != hit.message_revision) return true;
    if (hit.target == kUiPluginRenderShowTarget) {
        if (host->reveal(record->id)) {
            refresh_account_text(true);
            application_request_frame();
        }
        return true;
    }
    UiPluginRenderDecoration decoration{};
    if (!g_ui_plugin_render_query || !g_ui_plugin_render_query(record->id, record->revision, &decoration)) return false;
    UiPluginRenderNode node{};
    bool found_node = false;
    if (hit.target >= kUiPluginRenderCardTarget && decoration.has_card) {
        if (hit.target == kUiPluginRenderCardLinkTarget) {
            if (decoration.card.link.bytes) application_open_content_url(decoration.card.link);
            return true;
        }
        const u64 index = hit.target - kUiPluginRenderCardTarget;
        if (index >= kUiPluginRenderCardButtons || !decoration.card.buttons[index].label.bytes) return true;
        return application_plugin_surfaces_activate(decoration.card.owner_id,
                                                    {kUiPluginSlotMessageAccessory, record->channel_id, record->id, 0, 0},
                                                    decoration.card.buttons[index].action_id);
    }
    if (hit.target && hit.target <= decoration.accessory_count) {
        node = decoration.accessories[hit.target - 1];
        found_node = true;
    }
    if (!found_node) return false;
    if (node.kind == kUiPluginRenderLink) return node.value.bytes ? application_open_content_url(node.value) : true;
    if (node.kind != kUiPluginRenderButton) return true;
    return application_plugin_surfaces_activate(node.owner_id,
                                                {kUiPluginSlotMessageAccessory, record->channel_id, record->id, 0, 0},
                                                node.action_id);
}

void application_plugin_surfaces_context_changed() noexcept {
    if (!g_app.account_ui) return;
    const AccountUiRuntime& ui = *g_app.account_ui;
    g_host.context_channel = ui.active_channel_id;
    if (const PluginSurfaceHost* host = g_plugin_surfaces)
        host->refresh_buttons(ui.active_channel_id, guild_of_channel(ui, ui.active_channel_id));
}

} // namespace maicord::app_detail
