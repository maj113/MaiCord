// mc_api's cache reads (read_user, read_channel, read_guild, read_permissions, read_voice_channel and
// read_shared_group_dms): copies from MaiCord's cache through the application's reader (surfaces.hpp).
// Nothing here fetches.
#include "surfaces_internal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::plugin_surfaces_detail {

static_assert(kPluginMessageOwn == MC_MESSAGE_OWN && kPluginMessageBot == MC_MESSAGE_BOT &&
              kPluginMessageEdited == MC_MESSAGE_EDITED && kPluginMessagePinned == MC_MESSAGE_PINNED &&
              kPluginMessageReply == MC_MESSAGE_REPLY && kPluginMessageMedia == MC_MESSAGE_MEDIA &&
              kPluginMessageSystem == MC_MESSAGE_SYSTEM && kPluginMessageDeleted == MC_MESSAGE_DELETED &&
              kPluginMessageLocal == MC_MESSAGE_LOCAL && kPluginMessagePrivate == MC_MESSAGE_PRIVATE &&
              kPluginMessageNsfw == MC_MESSAGE_NSFW);
static_assert(kPluginRelationshipFriend == MC_RELATIONSHIP_FRIEND && kPluginRelationshipBlocked == MC_RELATIONSHIP_BLOCKED &&
              kPluginRelationshipIncoming == MC_RELATIONSHIP_INCOMING &&
              kPluginRelationshipOutgoing == MC_RELATIONSHIP_OUTGOING);
static_assert(kPluginNeedContent == MC_NEED_CONTENT && kPluginNeedAuthor == MC_NEED_AUTHOR);
static_assert(kPluginUserMemberRow == MC_USER_MEMBER_ROW && kPluginUserMessageAuthor == MC_USER_MESSAGE_AUTHOR &&
              kPluginUserProfile == MC_USER_PROFILE && kPluginUserBot == MC_USER_BOT && kPluginUserSelf == MC_USER_SELF &&
              kPluginPresenceOnline == MC_PRESENCE_ONLINE && kPluginPresenceOffline == MC_PRESENCE_OFFLINE &&
              kPluginPlatformDesktop == MC_PLATFORM_DESKTOP && kPluginPlatformMobile == MC_PLATFORM_MOBILE &&
              kPluginPlatformWeb == MC_PLATFORM_WEB);
static_assert(kPluginChannelNsfw == MC_CHANNEL_NSFW && kPluginChannelUnread == MC_CHANNEL_UNREAD &&
              kPluginChannelMuted == MC_CHANNEL_MUTED && kPluginChannelPrivate == MC_CHANNEL_PRIVATE &&
              kPluginChannelCanSend == MC_CHANNEL_CAN_SEND && kPluginGuildUnread == MC_GUILD_UNREAD &&
              kPluginGuildMuted == MC_GUILD_MUTED && kPluginHeaderChannel == MC_HEADER_CHANNEL &&
              kPluginHeaderMemberList == MC_HEADER_MEMBER_LIST);

namespace {
mc_str view(Utf8View text) noexcept { return {text.data, text.bytes, 0}; }
} // namespace

mc_status read_user(u64 user_id, u64 guild_id, mc_user* out) noexcept {
    const PluginReader* reader = g_plugin_reader;
    if (!out || !user_id) return MC_E_INVALID;
    if (!reader || !reader->user) return MC_E_UNSUPPORTED;
    PluginUserInput in{};
    if (!reader->user(user_id, guild_id, &in)) return MC_E_NOT_FOUND;
    *out = {};
    out->user_id = in.user_id;
    out->guild_id = in.guild_id;
    out->channel_id = in.channel_id;
    out->flags = in.flags;
    out->relationship = in.relationship;
    out->presence = in.presence;
    out->platforms = in.platforms;
    out->name = view(in.name);
    out->username = view(in.username);
    return MC_OK;
}

mc_status read_channel(u64 channel_id, mc_channel* out) noexcept {
    const PluginReader* reader = g_plugin_reader;
    if (!out || !channel_id) return MC_E_INVALID;
    if (!reader || !reader->channel) return MC_E_UNSUPPORTED;
    PluginChannelInput in{};
    if (!reader->channel(channel_id, &in)) return MC_E_NOT_FOUND;
    *out = {};
    out->channel_id = in.channel_id;
    out->guild_id = in.guild_id;
    out->parent_id = in.parent_id;
    out->self_permissions = in.self_permissions;
    out->type = in.type;
    out->flags = in.flags;
    out->mention_count = in.mention_count;
    out->name = view(in.name);
    return MC_OK;
}

mc_status read_guild(u64 guild_id, mc_guild* out) noexcept {
    const PluginReader* reader = g_plugin_reader;
    if (!out || !guild_id) return MC_E_INVALID;
    if (!reader || !reader->guild) return MC_E_UNSUPPORTED;
    PluginGuildInput in{};
    if (!reader->guild(guild_id, &in)) return MC_E_NOT_FOUND;
    *out = {};
    out->guild_id = in.guild_id;
    out->owner_id = in.owner_id;
    out->self_permissions = in.self_permissions;
    out->flags = in.flags;
    out->mention_count = in.mention_count;
    out->member_count = in.member_count;
    out->online_count = in.online_count;
    out->name = view(in.name);
    out->boost_count = in.boost_count;
    out->boost_tier = in.boost_tier;
    out->verification_level = in.verification_level;
    out->channel_count = in.channel_count;
    out->role_count = in.role_count;
    out->emoji_count = in.emoji_count;
    return MC_OK;
}

mc_status read_permissions(u64 guild_id, u64 channel_id, u64 user_id, u64* bits) noexcept {
    const PluginReader* reader = g_plugin_reader;
    if (!bits || !guild_id || !user_id) return MC_E_INVALID;
    *bits = 0;
    if (!reader || !reader->permissions) return MC_E_UNSUPPORTED;
    return reader->permissions(guild_id, channel_id, user_id, bits) ? MC_OK : MC_E_NOT_FOUND;
}

mc_status read_voice_channel(u64 user_id, u64 guild_id, u64* channel) noexcept {
    const PluginReader* reader = g_plugin_reader;
    if (!channel || !user_id) return MC_E_INVALID;
    *channel = 0;
    if (!reader || !reader->voice_channel) return MC_E_UNSUPPORTED;
    return reader->voice_channel(user_id, guild_id, channel) && *channel ? MC_OK : MC_E_NOT_FOUND;
}

mc_status read_shared_group_dms(u64 user_id, u64* out, u32 capacity, u32* count) noexcept {
    const PluginReader* reader = g_plugin_reader;
    if (!count || !user_id || (capacity && !out)) return MC_E_INVALID;
    *count = 0;
    if (!reader || !reader->shared_group_dms) return MC_E_UNSUPPORTED;
    *count = reader->shared_group_dms(user_id, {out, capacity});
    return *count ? MC_OK : MC_E_NOT_FOUND;
}

} // namespace maicord::plugin_surfaces_detail
