// Client state for the plugin SDK: mc.read sources and the gateway event feed.
// Everything here copies out of resident stores on the UI thread; nothing
// fetches, allocates or keeps a view past the call.
#include "application_plugins.hpp"
#include "application_plugins_runtime.hpp"
#include "application_friends.hpp"
#include "application_profile.hpp"
#include "application_state.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/relationship_store.hpp"
#include "maicord/protocol/client_status.hpp"

namespace maicord::app_detail {
namespace {

const AccountUiRuntime* ui_state() noexcept { return g_app.account_ui; }
u64 self_id() noexcept { return g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0; }

u32 status_from_member(GuildMemberStatus status) noexcept {
    switch (status) {
    case GuildMemberStatus::online: return MC_STATUS_ONLINE;
    case GuildMemberStatus::idle: return MC_STATUS_IDLE;
    case GuildMemberStatus::dnd: return MC_STATUS_DND;
    case GuildMemberStatus::offline: return MC_STATUS_OFFLINE;
    default: return MC_STATUS_UNKNOWN;
    }
}
u32 status_from_relationship(RelationshipPresence presence) noexcept {
    switch (presence) {
    case RelationshipPresence::online: return MC_STATUS_ONLINE;
    case RelationshipPresence::idle: return MC_STATUS_IDLE;
    case RelationshipPresence::dnd: return MC_STATUS_DND;
    case RelationshipPresence::offline: return MC_STATUS_OFFLINE;
    default: return MC_STATUS_UNKNOWN;
    }
}
u32 status_from_platform(ClientPlatformStatus status) noexcept {
    switch (status) {
    case ClientPlatformStatus::online: return MC_STATUS_ONLINE;
    case ClientPlatformStatus::idle: return MC_STATUS_IDLE;
    case ClientPlatformStatus::dnd: return MC_STATUS_DND;
    default: return MC_STATUS_OFFLINE;
    }
}
u32 status_from_list(GatewayClientMemberListStatus status) noexcept {
    switch (status) {
    case GatewayClientMemberListStatus::online: return MC_STATUS_ONLINE;
    case GatewayClientMemberListStatus::idle: return MC_STATUS_IDLE;
    case GatewayClientMemberListStatus::dnd: return MC_STATUS_DND;
    case GatewayClientMemberListStatus::offline: return MC_STATUS_OFFLINE;
    default: return MC_STATUS_UNKNOWN;
    }
}
u32 relationship_kind(RelationshipType type) noexcept {
    switch (type) {
    case RelationshipType::friend_: return MC_RELATIONSHIP_FRIEND;
    case RelationshipType::blocked: return MC_RELATIONSHIP_BLOCKED;
    case RelationshipType::incoming: return MC_RELATIONSHIP_INCOMING;
    case RelationshipType::outgoing: return MC_RELATIONSHIP_OUTGOING;
    case RelationshipType::implicit: return MC_RELATIONSHIP_IMPLICIT;
    default: return MC_RELATIONSHIP_NONE;
    }
}
// VoiceFlag (Gateway order: deaf, mute, self deaf, self mute) to MC_VOICE_*.
u32 voice_flags(u32 flags) noexcept {
    u32 out = 0;
    if (flags & voice_mute) out |= MC_VOICE_MUTE;
    if (flags & voice_deaf) out |= MC_VOICE_DEAF;
    if (flags & voice_self_mute) out |= MC_VOICE_SELF_MUTE;
    if (flags & voice_self_deaf) out |= MC_VOICE_SELF_DEAF;
    if (flags & voice_self_stream) out |= MC_VOICE_STREAMING;
    if (flags & voice_self_video) out |= MC_VOICE_VIDEO;
    if (flags & voice_suppress) out |= MC_VOICE_SUPPRESSED;
    return out;
}
const RelationshipRecord* relationship_of(u64 user) noexcept {
    const auto* store = application_friends_relationships();
    return store ? relationship_store_find(*store, user) : nullptr;
}
u64 guild_of_channel(const AccountUiRuntime& ui, u64 channel_id) noexcept {
    const auto* channel = channel_id ? chat_state_store_find_channel(ui.chat_state, channel_id) : nullptr;
    return channel ? channel->guild_id : 0;
}
// Server nickname when the member is resident, else the user's display name.
Utf8View display_name(const AccountUiRuntime& ui, u64 guild, u64 user) noexcept {
    if (const auto* member = guild && user ? guild_member_store_find(ui.guild_member_store, guild, user) : nullptr) {
        const auto nick = guild_member_store_nickname(*member);
        if (nick.bytes) return nick;
    }
    const auto* record = user ? user_store_find(ui.user_store, user) : nullptr;
    return record ? user_store_name(ui.user_store, *record) : Utf8View{};
}
bool user_is_bot(const AccountUiRuntime& ui, u64 user) noexcept {
    const auto* appearance = user ? user_appearance_find(ui.user_store.appearance, user) : nullptr;
    return appearance && (appearance->fields & appearance_bot) && appearance->bot;
}
const MessageRecord* cached_message(u64 channel, u64 message, MessageHandle* handle = nullptr) noexcept {
    const auto& store = g_app.message_view.store;
    const auto found = message_store_find(store, message);
    const auto* record = found.ok() ? message_store_resolve(store, found.handle) : nullptr;
    if (!record || (channel && record->channel_id != channel)) return nullptr;
    if (handle) *handle = found.handle;
    return record;
}
const MessageRichRecord* rich_of(const AccountUiRuntime& ui, u64 channel, u64 message) noexcept {
    return cached_message(channel, message) ? message_rich_store_find(ui.message_rich_store, message) : nullptr;
}
Utf8View rich_text(const AccountUiRuntime& ui, MessageRichString value) noexcept {
    return message_rich_store_string(ui.message_rich_store, value);
}

// ---- mc.read sources -------------------------------------------------------------------

bool read_context_info(void*, mc_context_info* info) noexcept {
    const auto* state = ui_state();
    if (!state) return false;
    const auto& ui = *state;
    info->channel_id = ui.active_channel_id;
    const auto* channel = info->channel_id ? chat_state_store_find_channel(ui.chat_state, info->channel_id) : nullptr;
    info->guild_id = channel ? channel->guild_id : 0;
    const Utf8View channel_name = channel ? chat_state_store_channel_name(ui.chat_state, *channel) : Utf8View{};
    info->channel_name_bytes = utf8_copy_truncated(info->channel_name, channel_name);
    info->user_id = ui.chat_state.self_user_id;
    const auto* user = info->user_id ? user_store_find(ui.user_store, info->user_id) : nullptr;
    info->user_name_bytes = utf8_copy_truncated(info->user_name, user ? user_store_name(ui.user_store, *user) : Utf8View{});
    return true;
}

bool read_guild(void*, u64 guild_id, mc_guild_info* info) noexcept {
    const auto* state = ui_state();
    const auto* guild = state ? chat_state_store_find_guild(state->chat_state, guild_id) : nullptr;
    if (!guild) return false;
    const auto& ui = *state;
    info->guild_id = guild->id;
    info->owner_id = guild->owner_id;
    info->created_ms = snowflake_timestamp_ms(guild->id);
    info->self_permissions = guild->base_permissions;
    if (guild->unavailable) info->flags |= MC_GUILD_UNAVAILABLE;
    if (guild->owner_id && guild->owner_id == ui.chat_state.self_user_id) info->flags |= MC_GUILD_OWNER;
    if (guild->upload_features & guild_record_community) info->flags |= MC_GUILD_COMMUNITY;
    // Each member list scope carries the server's counts; prefer the largest known.
    bool counts = false;
    for (const auto& list : ui.guild_member_store.lists) {
        if (list.guild_id != guild->id || !list.member_count_known) continue;
        if (!counts || list.member_count > info->member_count) {
            info->member_count = list.member_count;
            info->online_count = list.online_count_known ? list.online_count : 0;
        }
        counts = true;
    }
    if (counts) info->flags |= MC_GUILD_COUNTS;
    info->name_bytes = utf8_copy_truncated(info->name, chat_state_store_guild_name(ui.chat_state, *guild));
    info->icon_bytes = utf8_copy_truncated(info->icon_hash, chat_state_store_guild_icon_hash(*guild));
    return true;
}

bool read_channel(void*, u64 channel_id, mc_channel_info* info) noexcept {
    const auto* state = ui_state();
    const auto* channel = state ? chat_state_store_find_channel(state->chat_state, channel_id) : nullptr;
    if (!channel) return false;
    const auto& ui = *state;
    info->type = channel->type;
    info->channel_id = channel->id;
    info->guild_id = channel->guild_id;
    info->parent_id = channel->parent_id;
    info->last_message_id = channel->last_message_id;
    info->position = channel_is_thread(channel->type) ? 0 : channel->position;
    if (channel->permissions_resolved) {
        info->flags |= MC_CHANNEL_PERMISSIONS;
        info->self_permissions = channel->effective_permissions;
    }
    if (chat_state_store_channel_age_restricted(ui.chat_state, *channel)) info->flags |= MC_CHANNEL_NSFW;
    if (channel_is_thread(channel->type)) info->flags |= MC_CHANNEL_THREAD;
    if (channel->actions & channel_action_send_messages) info->flags |= MC_CHANNEL_CAN_SEND;
    Utf8View name = chat_state_store_channel_name(ui.chat_state, *channel);
    if (channel_is_private(channel->type)) {
        info->flags |= MC_CHANNEL_PRIVATE;
        const auto* privates = ui.chat_state.private_channels;
        const auto* record = privates ? private_channel_store_find(*privates, channel->id) : nullptr;
        const auto recipients = record ? private_channel_store_recipients(*privates, *record) : ConstSlice<u64>{};
        if (channel->type == 1 && recipients.count) {
            info->recipient_id = recipients[0];
            if (!name.bytes) name = display_name(ui, 0, info->recipient_id);
        }
    }
    info->name_bytes = utf8_copy_truncated(info->name, name);
    return true;
}

bool read_role(void*, u64 guild_id, u64 role_id, mc_role_info* info) noexcept {
    const auto* state = ui_state();
    const auto* role = state ? chat_state_store_find_role(state->chat_state, guild_id, role_id) : nullptr;
    if (!role) return false;
    info->role_id = role->id;
    info->guild_id = role->guild_id;
    info->permissions = role->permissions;
    info->colour = role->color & 0xFFFFFFu;
    info->position = role->position;
    if (role->hoist) info->flags |= MC_ROLE_HOIST;
    if (role->managed) info->flags |= MC_ROLE_MANAGED;
    if (role->mentionable) info->flags |= MC_ROLE_MENTIONABLE;
    info->name_bytes = utf8_copy_truncated(info->name, chat_state_store_role_name(state->chat_state, *role));
    return true;
}

bool read_member(void*, u64 guild_id, u64 user_id, mc_member_info* info) noexcept {
    const auto* state = ui_state();
    const auto* member = state ? guild_member_store_find(state->guild_member_store, guild_id, user_id) : nullptr;
    if (!member) return false;
    info->guild_id = member->guild_id;
    info->user_id = member->user_id;
    info->colour_role_id = member->color_role_id;
    info->status = status_from_member(member->status);
    if (member->roles_known) {
        info->flags |= MC_MEMBER_ROLES;
        const auto roles = guild_member_store_role_ids(*member);
        const u32 count = minimum<u32>(roles.count, 16);
        for (u32 i = 0; i < count; ++i) info->role_ids[i] = roles[i];
        info->role_count = count;
        if (member->truncated_roles || roles.count > 16) info->flags |= MC_MEMBER_TRUNCATED;
    }
    if (member->communication_disabled_known && member->communication_disabled_until) {
        const u64 until = static_cast<u64>(member->communication_disabled_until) * 1000u;
        if (until > clock_unix_milliseconds()) {
            info->timeout_until_ms = until;
            info->flags |= MC_MEMBER_TIMED_OUT;
        }
    }
    info->nick_bytes = utf8_copy_truncated(info->nick, guild_member_store_nickname(*member));
    return true;
}

bool read_user(void*, u64 user_id, mc_user_info* info) noexcept {
    const auto* state = ui_state();
    const auto* user = state ? user_store_find(state->user_store, user_id) : nullptr;
    if (!user) return false;
    info->user_id = user->id;
    if (user->id == state->chat_state.self_user_id) info->flags |= MC_USER_SELF;
    info->name_bytes = utf8_copy_truncated(info->name, user_store_name(state->user_store, *user));
    info->username_bytes = utf8_copy_truncated(info->username, user_store_username(state->user_store, *user));
    info->avatar_bytes = utf8_copy_truncated(info->avatar_hash, user_store_avatar_hash(*user));
    return true;
}

bool read_relationship(void*, u64 user_id, mc_relationship_info* info) noexcept {
    const auto* record = relationship_of(user_id);
    if (!record || record->type == RelationshipType::none || record->type == RelationshipType::suggestion) return false;
    info->type = relationship_kind(record->type);
    info->user_id = record->user_id;
    if (record->type == RelationshipType::friend_) info->status = status_from_relationship(record->presence);
    info->nickname_bytes = utf8_copy_truncated(info->nickname, Utf8View{record->nickname, record->nickname_bytes});
    return true;
}

bool read_profile(void*, u64 user_id, mc_profile_info* info) noexcept {
    const auto* profile = g_app.profile;
    if (!profile || !profile->scene.active || profile->pending || profile->data.user_id != user_id) return false;
    const auto& data = profile->data;
    info->user_id = data.user_id;
    info->guild_id = data.guild_id;
    u64 joined = 0;
    if (data.guild_id && data.joined_at.bytes && parse_iso_timestamp({data.joined_at.view(), 0, false}, &joined))
        info->joined_ms = joined;
    const auto pronouns = data.guild_id && data.guild_pronouns.bytes ? data.guild_pronouns.view() : data.pronouns.view();
    const auto bio = data.guild_id && data.guild_bio.bytes ? data.guild_bio.view() : data.bio.view();
    info->pronouns_bytes = utf8_copy_truncated(info->pronouns, pronouns);
    info->bio_bytes = utf8_copy_truncated(info->bio, bio);
    return true;
}

bool read_presence(void*, u64 user_id, u64 guild_id, mc_presence_info* info) noexcept {
    const auto* state = ui_state();
    if (!state) return false;
    const auto& ui = *state;
    const auto* member = guild_id ? guild_member_store_find(ui.guild_member_store, guild_id, user_id) : nullptr;
    const auto* relationship = relationship_of(user_id);
    const bool friend_known = relationship && relationship->type == RelationshipType::friend_ &&
                              relationship->presence != RelationshipPresence::unknown;
    const u32 member_status = member ? status_from_member(member->status) : MC_STATUS_UNKNOWN;
    u32 status = member_status ? member_status : friend_known ? status_from_relationship(relationship->presence) : 0;
    if (!status && !guild_id) {
        // Without a server, any resident member row carries the global status.
        for (u32 i = 0; i < ui.guild_member_store.capacity && !status; ++i) {
            const auto& row = ui.guild_member_store.records[i];
            if (row.occupied && row.user_id == user_id) status = status_from_member(row.status);
        }
    }
    const u8 packed = user_store_client_status(ui.user_store, user_id);
    if (!status && packed) {
        for (u32 platform = 0; platform < kClientPlatformCount && !status; ++platform) {
            const auto value = client_status_get(packed, static_cast<ClientPlatform>(platform));
            if (value != ClientPlatformStatus::offline) status = status_from_platform(value);
        }
    }
    const u32 activities = user_activity_count(ui.user_store.activities, user_id);
    const bool resident = user_store_find(ui.user_store, user_id) != nullptr;
    if (!status && !activities && !member && !friend_known && !resident) return false;
    info->status = status;
    info->user_id = user_id;
    info->desktop = status_from_platform(client_status_get(packed, ClientPlatform::desktop));
    info->mobile = status_from_platform(client_status_get(packed, ClientPlatform::mobile));
    info->web = status_from_platform(client_status_get(packed, ClientPlatform::web));
    info->activity_count = activities;
    return true;
}

bool read_activity(void*, u64 user_id, u32 index, mc_activity_info* info) noexcept {
    const auto* state = ui_state();
    UserActivitySet set;
    if (!state || !user_activity_find(state->user_store.activities, user_id, &set) || index >= set.count ||
        index >= kUserActivityMaximum) return false;
    const auto& activity = set.items[index];
    info->type = static_cast<u32>(activity.type) <= static_cast<u32>(MC_ACTIVITY_COMPETING) ? static_cast<u32>(activity.type)
                                                                          : MC_ACTIVITY_PLAYING;
    info->application_id = activity.application_id;
    info->start_ms = activity.start_ms;
    info->end_ms = activity.end_ms;
    info->name_bytes = utf8_copy_truncated(info->name, user_activity_text(activity, UserActivityField::name));
    info->details_bytes = utf8_copy_truncated(info->details, user_activity_text(activity, UserActivityField::details));
    info->state_bytes = utf8_copy_truncated(info->state, user_activity_text(activity, UserActivityField::state));
    return true;
}

bool read_voice(void*, u64 guild_id, u64 user_id, mc_voice_info* info) noexcept {
    const auto* state = ui_state();
    if (!state) return false;
    const auto& store = state->voice_states;
    const VoiceState* found = nullptr;
    if (guild_id) {
        found = voice_state_store_find(store, guild_id, user_id);
    } else {
        for (u32 i = 0; i < store.count && !found; ++i)
            if (store.records[i].user_id == user_id) found = &store.records[i];
    }
    if (!found || !found->channel_id) return false;
    info->flags = voice_flags(found->flags);
    info->guild_id = found->guild_id;
    info->channel_id = found->channel_id;
    info->user_id = found->user_id;
    return true;
}

bool read_message(void*, u64 channel_id, u64 message_id, mc_message_info* info, Utf8View* content) noexcept {
    const auto* state = ui_state();
    MessageHandle handle{};
    const auto* record = state ? cached_message(channel_id, message_id, &handle) : nullptr;
    if (!record) return false;
    const auto& ui = *state;
    info->channel_id = record->channel_id;
    info->message_id = record->id;
    info->author_id = record->author_id;
    info->guild_id = guild_of_channel(ui, record->channel_id);
    info->timestamp_ms = record->timestamp_ms;
    info->edited_ms = record->edited_timestamp_ms;
    info->type = record->type;
    info->revision = record->revision;
    if (record->author_id && record->author_id == ui.chat_state.self_user_id) info->flags |= MC_MESSAGE_OWN;
    if (user_is_bot(ui, record->author_id)) info->flags |= MC_MESSAGE_BOT;
    if (record->edited_timestamp_ms) info->flags |= MC_MESSAGE_EDITED;
    if (record->presentation_flags & message_presentation_pinned) info->flags |= MC_MESSAGE_PINNED;
    if (const auto* rich = message_rich_store_find(ui.message_rich_store, record->id)) {
        info->attachment_count = rich->attachment_count;
        info->embed_count = rich->embed_count;
        info->reaction_count = rich->reaction_count;
        // Reference type 0 is a reply; type 1 is a forward.
        if (!rich->forwarded_snapshot_child && rich->reference.type == 0 && rich->reference.message_id) {
            info->reply_message_id = rich->reference.message_id;
            info->reply_channel_id = rich->reference.channel_id ? rich->reference.channel_id : record->channel_id;
            info->flags |= MC_MESSAGE_REPLY;
        }
        if (rich->attachment_count || rich->embed_count) info->flags |= MC_MESSAGE_MEDIA;
    }
    info->author_name_bytes = utf8_copy_truncated(info->author_name, display_name(ui, info->guild_id, record->author_id));
    *content = record->content_availability == MessageContentAvailability::available
                   ? message_store_content(g_app.message_view.store, handle)
                   : Utf8View{};
    return true;
}

bool read_attachment(void*, u64 channel_id, u64 message_id, u32 index, mc_attachment_info* info) noexcept {
    const auto* state = ui_state();
    const auto* rich = state ? rich_of(*state, channel_id, message_id) : nullptr;
    if (!rich || index >= rich->attachment_count) return false;
    const auto& ui = *state;
    const auto& attachment = rich->attachments[index];
    const auto filename = rich_text(ui, attachment.filename);
    constexpr auto spoiler = utf8_literal("SPOILER_");
    if ((attachment.flags & kGatewayAttachmentSpoilerFlag) ||
        (filename.bytes >= spoiler.bytes && bytes_equal(filename.data, spoiler.data, spoiler.bytes)))
        info->flags |= MC_ATTACHMENT_SPOILER;
    info->attachment_id = attachment.id;
    info->bytes = attachment.size;
    info->width = attachment.width;
    info->height = attachment.height;
    info->filename_bytes = utf8_copy_truncated(info->filename, filename);
    info->content_type_bytes = utf8_copy_truncated(info->content_type, rich_text(ui, attachment.content_type));
    info->url_bytes = utf8_copy_truncated(info->url, rich_text(ui, attachment.url));
    return true;
}

bool read_embed(void*, u64 channel_id, u64 message_id, u32 index, mc_embed_info* info) noexcept {
    const auto* state = ui_state();
    const auto* rich = state ? rich_of(*state, channel_id, message_id) : nullptr;
    if (!rich || index >= rich->embed_count) return false;
    const auto& ui = *state;
    const auto& embed = rich->embeds[index];
    info->timestamp_ms = embed.timestamp_ms;
    info->title_bytes = utf8_copy_truncated(info->title, rich_text(ui, embed.title));
    info->url_bytes = utf8_copy_truncated(info->url, rich_text(ui, embed.url));
    info->description_bytes = utf8_copy_truncated(info->description, rich_text(ui, embed.description));
    info->provider_bytes = utf8_copy_truncated(info->provider, rich_text(ui, embed.provider_name));
    return true;
}

bool read_reaction(void*, u64 channel_id, u64 message_id, u32 index, mc_reaction_info* info) noexcept {
    const auto* state = ui_state();
    const auto* rich = state ? rich_of(*state, channel_id, message_id) : nullptr;
    if (!rich || index >= rich->reaction_count) return false;
    const auto& reaction = rich->reactions[index];
    // Rich store reaction flags: 1 me, 2 me (burst), 4 animated.
    if (reaction.flags & 3u) info->flags |= MC_REACTION_ME;
    if (reaction.flags & 4u) info->flags |= MC_REACTION_ANIMATED;
    info->emoji_id = reaction.emoji_id;
    info->count = reaction.count;
    info->name_bytes = utf8_copy_truncated(info->name, rich_text(*state, reaction.name));
    return true;
}

// Copies ids for indices [begin, end); id_at returns zero to skip an index.
template <typename IdAt>
void page(u32 begin, u32 end, Slice<u64> ids, u32* count, u32* next, IdAt id_at) noexcept {
    *count = 0;
    *next = 0;
    for (u32 i = begin; i < end; ++i) {
        const u64 id = id_at(i);
        if (!id) continue;
        if (*count >= ids.count) {
            *next = i;
            return;
        }
        ids[(*count)++] = id;
    }
}

bool read_list(void*, u32 list, u64 scope, u32 cursor, Slice<u64> ids, u32* count, u32* next) noexcept {
    const auto* state = ui_state();
    if (!state) return false;
    const auto& ui = *state;
    const auto& chat = ui.chat_state;
    switch (list) {
    case MC_LIST_GUILDS:
        page(cursor, chat.guild_count, ids, count, next, [&](u32 i) noexcept {
            const auto& guild = chat.guilds[chat.guild_order[i]];
            return guild.occupied ? guild.id : 0;
        });
        return true;
    case MC_LIST_CHANNELS:
        if (scope && !chat_state_store_find_guild(chat, scope)) return false;
        page(cursor, chat.channel_count, ids, count, next, [&](u32 i) noexcept {
            const auto& channel = chat.channels[chat.channel_order[i]];
            const bool match = scope ? channel.guild_id == scope : channel_is_private(channel.type);
            return channel.occupied && match ? channel.id : 0;
        });
        return true;
    case MC_LIST_ROLES:
        if (!scope || !chat_state_store_find_guild(chat, scope)) return false;
        page(cursor, chat.roles ? chat.role_count : 0, ids, count, next,
             [&](u32 i) noexcept { return chat.roles[i].guild_id == scope ? chat.roles[i].id : 0; });
        return true;
    case MC_LIST_MEMBERS: {
        if (!scope || !chat_state_store_find_guild(chat, scope)) return false;
        const auto& members = ui.guild_member_store;
        page(cursor, members.records ? members.capacity : 0, ids, count, next, [&](u32 i) noexcept {
            const auto& row = members.records[i];
            return row.occupied && row.guild_id == scope ? row.user_id : 0;
        });
        return true;
    }
    case MC_LIST_RELATIONSHIPS: {
        const auto* store = application_friends_relationships();
        if (!store) return false;
        page(cursor, store->count, ids, count, next, [&](u32 i) noexcept {
            const auto& row = store->records[i];
            return row.type != RelationshipType::none && row.type != RelationshipType::suggestion ? row.user_id : 0;
        });
        return true;
    }
    case MC_LIST_VOICE: {
        if (!scope) return false;
        const auto& voice = ui.voice_states;
        page(cursor, voice.count, ids, count, next, [&](u32 i) noexcept {
            return voice.records[i].channel_id == scope ? voice.records[i].user_id : 0;
        });
        return true;
    }
    case MC_LIST_MESSAGES: {
        if (!scope) return false;
        const auto& store = g_app.message_view.store;
        // Cursor k counts from the newest message (order is ascending by id).
        page(cursor, store.records ? store.count : 0, ids, count, next, [&](u32 k) noexcept {
            const auto* record = message_store_resolve(store, message_store_ordered_handle(store, store.count - 1 - k));
            return record && record->channel_id == scope ? record->id : 0;
        });
        return true;
    }
    default: return false;
    }
}

// Store counters are per class and may reset with the account; plugins see a
// host counter that only moves forward when the underlying mix changes.
struct RevisionTrack {
    u64 mix[5];
    u64 value[5];
};
RevisionTrack g_revisions;
template <typename... Parts> u64 mix(Parts... parts) noexcept {
    u64 hash = 0xcbf29ce484222325ull;
    ((hash = (hash ^ static_cast<u64>(parts)) * 0x100000001b3ull), ...);
    return hash;
}
void read_revisions(void*, mc_read_revisions* revisions) noexcept {
    const auto* state = ui_state();
    if (!state) return;
    const auto& ui = *state;
    const auto* relationships = application_friends_relationships();
    const u64 relationship_revision = relationships ? relationships->revision : 0;
    const auto& messages = g_app.message_view.store;
    const u64 current[5]{
        mix(ui.chat_state.guild_mutations, ui.chat_state.channel_mutations, ui.chat_state.permission_mutations,
            ui.chat_state.guild_count, ui.chat_state.channel_count),
        mix(ui.user_store.revision, ui.guild_member_store.revision, relationship_revision),
        mix(ui.guild_member_store.presence_revision, ui.user_store.activities.revision,
            ui.user_store.appearance.revision, relationship_revision),
        mix(ui.voice_states.revision, ui.voice_states.count),
        mix(messages.created, messages.updated, messages.deleted, ui.message_rich_store.updated,
            ui.message_rich_store.deleted),
    };
    for (u32 i = 0; i < 5; ++i) {
        if (g_revisions.mix[i] != current[i] || !g_revisions.value[i]) {
            g_revisions.mix[i] = current[i];
            ++g_revisions.value[i];
        }
    }
    revisions->guilds = g_revisions.value[0];
    revisions->members = g_revisions.value[1];
    revisions->presence = g_revisions.value[2];
    revisions->voice = g_revisions.value[3];
    revisions->messages = g_revisions.value[4];
}

// ---- Gateway translation ---------------------------------------------------------------------

u64 dispatch_kinds(GatewayClientDispatchKind kind) noexcept {
    using K = GatewayClientDispatchKind;
    switch (kind) {
    case K::message_create: return MC_EVENT_MASK(MC_EVENT_MESSAGE_CREATE);
    case K::message_update: return MC_EVENT_MASK(MC_EVENT_MESSAGE_UPDATE);
    case K::message_delete:
    case K::message_bulk_delete: return MC_EVENT_MASK(MC_EVENT_MESSAGE_DELETE);
    case K::typing_start: return MC_EVENT_MASK(MC_EVENT_TYPING);
    case K::message_reaction_add: return MC_EVENT_MASK(MC_EVENT_REACTION_ADD);
    case K::message_reaction_remove: return MC_EVENT_MASK(MC_EVENT_REACTION_REMOVE);
    case K::presence_update: return MC_EVENT_MASK(MC_EVENT_PRESENCE);
    case K::voice_state_snapshot: return MC_EVENT_MASK(MC_EVENT_VOICE_STATE);
    case K::channel_create: return MC_EVENT_MASK(MC_EVENT_CHANNEL_CREATE);
    case K::channel_update: return MC_EVENT_MASK(MC_EVENT_CHANNEL_UPDATE);
    case K::channel_delete: return MC_EVENT_MASK(MC_EVENT_CHANNEL_DELETE);
    case K::guild_create: return MC_EVENT_MASK(MC_EVENT_GUILD_CREATE);
    case K::guild_update: return MC_EVENT_MASK(MC_EVENT_GUILD_UPDATE);
    case K::guild_delete: return MC_EVENT_MASK(MC_EVENT_GUILD_DELETE);
    case K::guild_member_update: return MC_EVENT_MASK(MC_EVENT_MEMBER_UPDATE);
    case K::guild_member_profile_update: return MC_EVENT_MASK(MC_EVENT_MEMBER_ADD) | MC_EVENT_MASK(MC_EVENT_MEMBER_UPDATE);
    case K::guild_member_remove: return MC_EVENT_MASK(MC_EVENT_MEMBER_REMOVE);
    default: return 0;
    }
}

struct Emitter {
    u64 interest, self;
    void* context;
    PluginGatewayEmit emit;
    u32 count;
    void operator()(NativePluginSdkEvent& event) noexcept {
        if (!(interest & MC_EVENT_MASK(event.kind))) return;
        if (event.user_id && event.user_id == self) event.flags |= MC_EVENT_OWN;
        if (emit(context, event)) ++count;
    }
};

// Fills text/name/user from the cached copy of a message about to change.
void from_cache(const AccountUiRuntime* ui, NativePluginSdkEvent& out, bool text) noexcept {
    MessageHandle handle{};
    const auto* record = cached_message(out.channel_id, out.message_id, &handle);
    if (!record) return;
    if (!out.user_id) out.user_id = record->author_id;
    if (ui && !out.name.bytes) out.name = display_name(*ui, out.guild_id, record->author_id);
    if (text && record->content_availability == MessageContentAvailability::available)
        out.text = message_store_content(g_app.message_view.store, handle);
    if (out.name.bytes || out.text.bytes) out.flags |= MC_EVENT_CACHED;
    if (ui && user_is_bot(*ui, record->author_id)) out.flags |= MC_EVENT_BOT;
}

void translate_message(const GatewayClientEvent& event, ConstSlice<u8> payload, const AccountUiRuntime* ui,
                       Emitter& emit) noexcept {
    GatewayMessageEvent message{};
    if (!gateway_client_event_as_message(event, payload, &message)) return;
    const bool create = event.dispatch_kind == GatewayClientDispatchKind::message_create;
    NativePluginSdkEvent out{};
    out.kind = create ? MC_EVENT_MESSAGE_CREATE : MC_EVENT_MESSAGE_UPDATE;
    out.channel_id = message.channel_id;
    out.message_id = message.id;
    out.guild_id = ui ? guild_of_channel(*ui, message.channel_id) : 0;
    if (message.fields & gateway_message_field_author_id) {
        out.user_id = message.author_id;
        out.name = message.author_name;
        if (message.author_bot) out.flags |= MC_EVENT_BOT;
    }
    if (create || (message.fields & gateway_message_field_content)) {
        out.text = message.content;
        out.flags |= MC_EVENT_TEXT;
    }
    if (!create && !out.user_id) from_cache(ui, out, false);
    emit(out);
}

void translate_delete(const NativePluginSdkEvent& base, const AccountUiRuntime* ui, Emitter& emit) noexcept {
    NativePluginSdkEvent out = base;
    from_cache(ui, out, true);
    emit(out);
}

void translate_voice(const GatewayClientEvent& event, ConstSlice<u8> payload, const AccountUiRuntime* ui,
                     Emitter& emit) noexcept {
    const auto& snapshot = event.voice_states;
    // A guild snapshot is state discovery, not a change; plugins read it with mc.read.
    if (snapshot.replace || payload.count != snapshot.record_count * sizeof(GatewayClientVoiceStateRecord)) return;
    constexpr u32 kGatewayFlags = 0x7Fu;
    for (u32 i = 0; i < snapshot.record_count; ++i) {
        GatewayClientVoiceStateRecord record;
        copy_bytes(&record, payload.data + i * sizeof(record), sizeof(record));
        if (!record.user_id) continue;
        const auto* known = ui ? voice_state_store_find(ui->voice_states, record.guild_id, record.user_id) : nullptr;
        const u64 known_channel = known ? known->channel_id : 0;
        const u32 known_flags = known ? known->flags & kGatewayFlags : 0;
        if (known_channel == record.channel_id && known_flags == (record.flags & kGatewayFlags)) continue;
        NativePluginSdkEvent out{};
        out.kind = MC_EVENT_VOICE_STATE;
        out.guild_id = record.guild_id;
        out.channel_id = record.channel_id;
        out.user_id = record.user_id;
        out.value = record.channel_id ? voice_flags(record.flags) : 0;
        emit(out);
    }
}

void translate_state(const GatewayClientEvent& event, ConstSlice<u8> payload, const AccountUiRuntime* ui,
                     Emitter& emit) noexcept {
    using K = GatewayClientDispatchKind;
    const auto kind = event.dispatch_kind;
    const auto& state = event.state;
    NativePluginSdkEvent out{};
    Utf8View name{};
    if (gateway_client_event_state_name(event, payload, &name)) out.name = name;
    if (!state.id) return;
    if (kind == K::guild_create || kind == K::guild_update || kind == K::guild_delete) {
        out.kind = kind == K::guild_create ? MC_EVENT_GUILD_CREATE
                   : kind == K::guild_update ? MC_EVENT_GUILD_UPDATE
                                             : MC_EVENT_GUILD_DELETE;
        out.guild_id = state.id;
        if (!out.name.bytes && ui)
            if (const auto* guild = chat_state_store_find_guild(ui->chat_state, state.id))
                out.name = chat_state_store_guild_name(ui->chat_state, *guild);
    } else {
        out.kind = kind == K::channel_create ? MC_EVENT_CHANNEL_CREATE
                   : kind == K::channel_update ? MC_EVENT_CHANNEL_UPDATE
                                               : MC_EVENT_CHANNEL_DELETE;
        out.channel_id = state.id;
        out.guild_id = state.guild_id;
        out.value = state.type;
        const auto* known = ui ? chat_state_store_find_channel(ui->chat_state, state.id) : nullptr;
        if (known) {
            if (!(state.fields & gateway_client_state_field_guild_id)) out.guild_id = known->guild_id;
            if (!(state.fields & gateway_client_state_field_type)) out.value = known->type;
            if (!out.name.bytes) out.name = chat_state_store_channel_name(ui->chat_state, *known);
        }
    }
    emit(out);
}

void translate_member(const GatewayClientEvent& event, ConstSlice<u8> payload, const AccountUiRuntime* ui,
                      Emitter& emit) noexcept {
    using K = GatewayClientDispatchKind;
    NativePluginSdkEvent out{};
    if (event.dispatch_kind == K::guild_member_update) {
        // Your own member object (roles, timeout); others arrive as profile updates.
        out.kind = MC_EVENT_MEMBER_UPDATE;
        out.guild_id = event.state.guild_id;
        out.user_id = event.state.id;
    } else if (event.dispatch_kind == K::guild_member_remove) {
        Utf8View name{};
        if (!gateway_client_event_guild_membership(event, payload, &out.guild_id, &out.user_id, &name)) return;
        out.kind = MC_EVENT_MEMBER_REMOVE;
        out.name = name;
    } else {
        u32 cursor = 0, index = 0;
        GatewayClientMemberListMutation mutation{};
        ConstSlice<u8> roles{};
        Utf8View nickname{}, display{}, avatar{}, username{};
        if (event.member_list.record_count != 1 ||
            !gateway_client_event_member_list_record(event, payload, &cursor, &index, &mutation, &roles, &nickname,
                                                     &display, &avatar, &username))
            return;
        out.kind = event.member_list.member_added ? MC_EVENT_MEMBER_ADD : MC_EVENT_MEMBER_UPDATE;
        out.guild_id = event.member_list.guild_id;
        out.user_id = mutation.user_id;
        out.name = nickname.bytes ? nickname : display.bytes ? display : username;
    }
    if (!out.guild_id || !out.user_id) return;
    if (!out.name.bytes && ui) out.name = display_name(*ui, out.guild_id, out.user_id);
    emit(out);
}

} // namespace

u32 application_plugins_gateway_translate(const GatewayClientEvent& event, ConstSlice<u8> payload, u64 interest,
                                          void* context, PluginGatewayEmit emit_event) noexcept {
    using K = GatewayClientDispatchKind;
    if (event.kind != GatewayClientEventKind::dispatch || !emit_event || !(interest & dispatch_kinds(event.dispatch_kind)))
        return 0;
    const auto* ui = ui_state();
    Emitter emit{interest, self_id(), context, emit_event, 0};
    switch (event.dispatch_kind) {
    case K::message_create:
    case K::message_update: translate_message(event, payload, ui, emit); break;
    case K::message_delete: {
        GatewayMessageDeleteEvent message{};
        if (!gateway_client_event_as_message_delete(event, payload, &message)) break;
        NativePluginSdkEvent out{};
        out.kind = MC_EVENT_MESSAGE_DELETE;
        out.channel_id = message.channel_id;
        out.message_id = message.id;
        out.guild_id = ui ? guild_of_channel(*ui, message.channel_id) : 0;
        translate_delete(out, ui, emit);
        break;
    }
    case K::message_bulk_delete: {
        u64 ids[kGatewayBulkDeleteMaximumIds];
        GatewayMessageBulkDeleteEvent message{};
        if (!gateway_client_event_as_message_bulk_delete(event, payload, {ids, kGatewayBulkDeleteMaximumIds}, &message))
            break;
        NativePluginSdkEvent out{};
        out.kind = MC_EVENT_MESSAGE_DELETE;
        out.flags = MC_EVENT_BULK;
        out.channel_id = message.channel_id;
        out.guild_id = ui ? guild_of_channel(*ui, message.channel_id) : 0;
        for (u32 i = 0; i < message.id_count; ++i) {
            out.message_id = ids[i];
            translate_delete(out, ui, emit);
        }
        break;
    }
    case K::typing_start: {
        GatewayTypingStartEvent typing{};
        if (!gateway_client_event_as_typing_start(event, payload, &typing)) break;
        NativePluginSdkEvent out{};
        out.kind = MC_EVENT_TYPING;
        out.channel_id = typing.channel_id;
        out.user_id = typing.user_id;
        out.guild_id = typing.guild_id ? typing.guild_id : ui ? guild_of_channel(*ui, typing.channel_id) : 0;
        emit(out);
        break;
    }
    case K::message_reaction_add:
    case K::message_reaction_remove: {
        GatewayMessageReactionEvent reaction{};
        if (!gateway_client_event_as_message_reaction(event, payload, &reaction)) break;
        NativePluginSdkEvent out{};
        out.kind = event.dispatch_kind == K::message_reaction_add ? MC_EVENT_REACTION_ADD : MC_EVENT_REACTION_REMOVE;
        out.channel_id = reaction.channel_id;
        out.message_id = reaction.message_id;
        out.user_id = reaction.user_id;
        out.value = reaction.emoji_id;
        out.text = reaction.name;
        out.guild_id = ui ? guild_of_channel(*ui, reaction.channel_id) : 0;
        emit(out);
        break;
    }
    case K::presence_update: {
        const auto& presence = event.presence;
        if (!presence.user_id || presence.status > GatewayClientMemberListStatus::offline) break;
        NativePluginSdkEvent out{};
        out.kind = MC_EVENT_PRESENCE;
        out.guild_id = presence.guild_id;
        out.user_id = presence.user_id;
        out.value = status_from_list(presence.status);
        emit(out);
        break;
    }
    case K::voice_state_snapshot: translate_voice(event, payload, ui, emit); break;
    case K::channel_create:
    case K::channel_update:
    case K::channel_delete:
    case K::guild_create:
    case K::guild_update:
    case K::guild_delete: translate_state(event, payload, ui, emit); break;
    case K::guild_member_update:
    case K::guild_member_profile_update:
    case K::guild_member_remove: translate_member(event, payload, ui, emit); break;
    default: break;
    }
    return emit.count;
}

void application_plugins_gateway(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    auto* sdk = g_app.plugins ? g_app.plugins->sdk : nullptr;
    if (!sdk || event.kind != GatewayClientEventKind::dispatch || !dispatch_kinds(event.dispatch_kind)) return;
    const u64 interest = native_plugin_sdk_event_interest(sdk->runtime);
    if (!(interest & dispatch_kinds(event.dispatch_kind))) return;
    const auto push = [](void* runtime, const NativePluginSdkEvent& value) noexcept {
        return native_plugin_sdk_event_push(static_cast<NativePluginSdkRuntime*>(runtime), value);
    };
    if (application_plugins_gateway_translate(event, payload, interest, &sdk->runtime, push))
        sdk->events_pending = true;
}

void application_plugins_read_install(NativePluginSdkRuntime& runtime) noexcept {
    runtime.read = {};
    runtime.read.context_info = read_context_info;
    runtime.read.guild = read_guild;
    runtime.read.channel = read_channel;
    runtime.read.role = read_role;
    runtime.read.member = read_member;
    runtime.read.user = read_user;
    runtime.read.relationship = read_relationship;
    runtime.read.profile = read_profile;
    runtime.read.presence = read_presence;
    runtime.read.activity = read_activity;
    runtime.read.voice = read_voice;
    runtime.read.message = read_message;
    runtime.read.attachment = read_attachment;
    runtime.read.embed = read_embed;
    runtime.read.reaction = read_reaction;
    runtime.read.list = read_list;
    runtime.read.revisions = read_revisions;
}

} // namespace maicord::app_detail
