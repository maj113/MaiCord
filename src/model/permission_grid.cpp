#include "maicord/model/permission_grid.hpp"

namespace maicord {
namespace {
constexpr u8 G = permission_scope_guild, T = permission_scope_text, V = permission_scope_voice,
             S = permission_scope_stage, C = permission_scope_channels;
using PG = PermissionGroup;
#define L(text) utf8_literal(text)
constexpr PermissionGridEntry kEntries[]{
    {kPermissionViewChannel, L("View channels"), L("See channels and read the messages in them."),
     L("View channel"), L("See this channel and read its messages."), PG::general, G | C},
    {kPermissionManageChannels, L("Manage channels"), L("Create, edit and delete channels."), L("Manage channel"),
     L("Rename this channel and change its settings."), PG::general, G | C},
    {kPermissionManageRoles, L("Manage roles"), L("Create and edit roles below their highest role."),
     L("Manage permissions"), L("Change the overrides on this channel."), PG::general, G | C},
    {kPermissionCreateExpressions, L("Create expressions"), L("Add emoji, stickers and soundboard sounds."), {}, {},
     PG::general, G},
    {kPermissionManageExpressions, L("Manage expressions"), L("Edit and remove emoji, stickers and sounds."), {}, {},
     PG::general, G},
    {kPermissionViewAuditLog, L("View audit log"), L("See the record of changes made in this server."), {}, {},
     PG::general, G},
    {kPermissionViewGuildInsights, L("View server insights"), L("See server activity statistics."), {}, {},
     PG::general, G},
    {kPermissionManageWebhooks, L("Manage webhooks"), L("Create, edit and delete webhooks."), {}, {}, PG::general,
     G | T | V | S},
    {kPermissionManageGuild, L("Manage server"), L("Change the server name, system channel and other settings."),
     {}, {}, PG::general, G},
    {kPermissionCreateInvite, L("Create invite"), L("Invite new people to the server."), {}, {}, PG::membership,
     G | C},
    {kPermissionChangeNickname, L("Change nickname"), L("Change their own nickname in this server."), {}, {},
     PG::membership, G},
    {kPermissionManageNicknames, L("Manage nicknames"), L("Change other members' nicknames."), {}, {},
     PG::membership, G},
    {kPermissionKickMembers, L("Kick members"), L("Remove members from the server."), {}, {}, PG::membership, G},
    {kPermissionBanMembers, L("Ban members"), L("Ban members and delete their recent messages."), {}, {},
     PG::membership, G},
    {kPermissionModerateMembers, L("Time out members"), L("Stop members from chatting, reacting or speaking for a while."),
     {}, {}, PG::membership, G},
    {kPermissionSendMessages, L("Send messages and create posts"), L("Send messages and start forum posts."), {}, {},
     PG::text, G | C},
    {kPermissionSendMessagesInThreads, L("Send messages in threads"), L("Reply in threads and forum posts."), {}, {},
     PG::text, G | T},
    {kPermissionCreatePublicThreads, L("Create public threads"), L("Start threads everyone in the channel can see."),
     {}, {}, PG::text, G | T},
    {kPermissionCreatePrivateThreads, L("Create private threads"), L("Start threads open to invited members only."),
     {}, {}, PG::text, G | T},
    {kPermissionEmbedLinks, L("Embed links"), L("Show previews for links they post."), {}, {}, PG::text, G | C},
    {kPermissionAttachFiles, L("Attach files"), L("Upload files and media."), {}, {}, PG::text, G | C},
    {kPermissionAddReactions, L("Add reactions"), L("Add new reactions to messages."), {}, {}, PG::text, G | C},
    {kPermissionUseExternalEmojis, L("Use external emoji"), L("Use emoji from other servers."), {}, {}, PG::text,
     G | C},
    {kPermissionUseExternalStickers, L("Use external stickers"), L("Use stickers from other servers."), {}, {},
     PG::text, G | C},
    {kPermissionMentionEveryone, L("Mention everyone and all roles"),
     L("Notify everyone, online members and any role, including unmentionable ones."), {}, {}, PG::text, G | C},
    {kPermissionManageMessages, L("Manage messages"), L("Delete messages from others and remove their embeds."), {},
     {}, PG::text, G | C},
    {kPermissionPinMessages, L("Pin messages"), L("Pin and unpin messages."), {}, {}, PG::text, G | C},
    {kPermissionBypassSlowmode, L("Bypass slowmode"), L("Send messages without waiting for slowmode."), {}, {},
     PG::text, G | C},
    {kPermissionManageThreads, L("Manage threads and posts"), L("Rename, lock, archive and delete threads and posts."),
     {}, {}, PG::text, G | T},
    {kPermissionReadMessageHistory, L("Read message history"), L("Read messages sent before they joined the channel."),
     {}, {}, PG::text, G | C},
    {kPermissionSendSpeechMessages, L("Send text-to-speech messages"), L("Send messages that are read aloud."), {},
     {}, PG::text, G | C},
    {kPermissionSendVoiceMessages, L("Send voice messages"), L("Record and send voice clips."), {}, {}, PG::text,
     G | C},
    {kPermissionSendPolls, L("Create polls"), L("Start polls in chat."), {}, {}, PG::text, G | C},
    {kPermissionConnect, L("Connect"), L("Join voice channels and hear others."), {}, {}, PG::voice, G | V | S},
    {kPermissionSpeak, L("Speak"), L("Talk in voice channels."), {}, {}, PG::voice, G | V},
    {kPermissionStream, L("Video"), L("Share their camera or screen."), {}, {}, PG::voice, G | V},
    {kPermissionUseSoundboard, L("Use soundboard"), L("Play sounds from the soundboard."), {}, {}, PG::voice, G | V},
    {kPermissionUseExternalSounds, L("Use external sounds"), L("Play soundboard sounds from other servers."), {}, {},
     PG::voice, G | V},
    {kPermissionUseVoiceActivity, L("Use voice activity"), L("Talk without holding push-to-talk."), {}, {},
     PG::voice, G | V},
    {kPermissionPrioritySpeaker, L("Priority speaker"), L("Lower everyone else's volume while they talk."), {}, {},
     PG::voice, G | V},
    {kPermissionMuteMembers, L("Mute members"), L("Mute others in voice channels."), {}, {}, PG::voice, G | V | S},
    {kPermissionDeafenMembers, L("Deafen members"), L("Deafen others in voice channels."), {}, {}, PG::voice, G | V},
    {kPermissionMoveMembers, L("Move members"), L("Move or disconnect members in voice channels."), {}, {},
     PG::voice, G | V | S},
    {kPermissionSetVoiceChannelStatus, L("Set voice channel status"), L("Change the status shown on a voice channel."),
     {}, {}, PG::voice, G | V},
    {kPermissionUseApplicationCommands, L("Use application commands"), L("Use slash commands and app menus."), {}, {},
     PG::apps, G | C},
    {kPermissionUseActivities, L("Use activities"), L("Start and join activities."), {}, {}, PG::apps, G | T | V},
    {kPermissionUseExternalApps, L("Use external apps"), L("Let apps they added post public replies."), {}, {},
     PG::apps, G | C},
    {kPermissionRequestToSpeak, L("Request to speak"), L("Ask to speak in stage channels."), {}, {}, PG::stage, G | S},
    {kPermissionCreateEvents, L("Create events"), L("Schedule events."), {}, {}, PG::events, G | V | S},
    {kPermissionManageEvents, L("Manage events"), L("Edit and cancel any event."), {}, {}, PG::events, G | V | S},
    {kPermissionAdministrator, L("Administrator"),
     L("Grants every permission and ignores channel overrides. Give it only to people you trust."), {}, {},
     PG::advanced, G},
};
constexpr Utf8View kGroupLabels[]{L("General server"), L("Membership"), L("Text"),      L("Voice"),
                                  L("Apps"),           L("Stage"),      L("Events"),    L("Advanced")};
#undef L
static_assert(sizeof(kGroupLabels) / sizeof(kGroupLabels[0]) == static_cast<u32>(PermissionGroup::count));
} // namespace

ConstSlice<PermissionGridEntry> permission_grid_entries() noexcept {
    return {kEntries, sizeof(kEntries) / sizeof(kEntries[0])};
}

Utf8View permission_group_label(PermissionGroup group) noexcept {
    return group < PermissionGroup::count ? kGroupLabels[static_cast<u32>(group)] : Utf8View{};
}

u8 permission_grid_channel_scope(u8 type) noexcept {
    switch (type) {
    case 0:
    case 5:
    case 15:
    case 16:
        return permission_scope_text;
    case 2:
        return permission_scope_voice;
    case 13:
        return permission_scope_stage;
    case 4:
        return permission_scope_channels;
    default:
        return 0;
    }
}

bool permission_grid_applies(const PermissionGridEntry& entry, u8 scope) noexcept {
    return (entry.scopes & scope) != 0;
}

Utf8View permission_grid_label(const PermissionGridEntry& entry, u8 scope) noexcept {
    return !(scope & permission_scope_guild) && entry.channel_label.bytes ? entry.channel_label : entry.label;
}

Utf8View permission_grid_description(const PermissionGridEntry& entry, u8 scope) noexcept {
    return !(scope & permission_scope_guild) && entry.channel_description.bytes ? entry.channel_description
                                                                                : entry.description;
}

PermissionTriState permission_tri_get(PermissionSet allow, PermissionSet deny, PermissionSet bit) noexcept {
    return (allow & bit) ? PermissionTriState::allow
           : (deny & bit) ? PermissionTriState::deny
                          : PermissionTriState::neutral;
}

void permission_tri_set(PermissionSet* allow, PermissionSet* deny, PermissionSet bit,
                        PermissionTriState state) noexcept {
    *allow &= ~bit;
    *deny &= ~bit;
    if (state == PermissionTriState::allow)
        *allow |= bit;
    else if (state == PermissionTriState::deny)
        *deny |= bit;
}

bool permission_overwrite_draft_load(PermissionOverwriteDraft* draft,
                                     ConstSlice<PermissionOverwriteRecord> source) noexcept {
    if (!draft || source.count > kPermissionOverwriteDraftMaximum || (source.count && !source.data))
        return false;
    for (u32 i = 0; i < source.count; ++i)
        draft->entries[i] = source[i];
    draft->count = source.count;
    return true;
}

u32 permission_overwrite_draft_find(const PermissionOverwriteDraft& draft, u64 id) noexcept {
    for (u32 i = 0; i < draft.count; ++i)
        if (draft.entries[i].id == id)
            return i;
    return ~0u;
}

bool permission_overwrite_draft_add(PermissionOverwriteDraft* draft, u64 id, u8 type) noexcept {
    if (!draft || !id || type > 1 || draft->count >= kPermissionOverwriteDraftMaximum ||
        permission_overwrite_draft_find(*draft, id) != ~0u)
        return false;
    draft->entries[draft->count++] = {id, 0, 0, type};
    return true;
}

bool permission_overwrite_draft_remove(PermissionOverwriteDraft* draft, u64 id) noexcept {
    const u32 index = draft ? permission_overwrite_draft_find(*draft, id) : ~0u;
    if (index == ~0u)
        return false;
    for (u32 i = index + 1; i < draft->count; ++i)
        draft->entries[i - 1] = draft->entries[i];
    --draft->count;
    return true;
}

bool permission_overwrite_draft_set(PermissionOverwriteDraft* draft, u64 id, PermissionSet bit,
                                    PermissionTriState state) noexcept {
    const u32 index = draft ? permission_overwrite_draft_find(*draft, id) : ~0u;
    if (index == ~0u || !bit || state > PermissionTriState::allow)
        return false;
    // Packed records are copied out and back to avoid unaligned references.
    auto entry = draft->entries[index];
    PermissionSet allow = entry.allow, deny = entry.deny;
    permission_tri_set(&allow, &deny, bit, state);
    entry.allow = allow;
    entry.deny = deny;
    draft->entries[index] = entry;
    return true;
}

bool permission_overwrite_draft_equal(const PermissionOverwriteDraft& draft,
                                      ConstSlice<PermissionOverwriteRecord> source) noexcept {
    if (draft.count != source.count)
        return false;
    for (u32 i = 0; i < source.count; ++i) {
        const u32 index = permission_overwrite_draft_find(draft, source[i].id);
        if (index == ~0u)
            return false;
        const auto entry = draft.entries[index];
        if (entry.allow != source[i].allow || entry.deny != source[i].deny || entry.type != source[i].type)
            return false;
    }
    return true;
}

u32 permission_overwrite_draft_write(const PermissionOverwriteDraft& draft,
                                     Slice<PermissionOverwriteWrite> output) noexcept {
    if (draft.count > output.count || (draft.count && !output.data))
        return ~0u;
    for (u32 i = 0; i < draft.count; ++i) {
        const auto entry = draft.entries[i];
        output[i] = {entry.id, entry.allow, entry.deny, entry.type};
    }
    return draft.count;
}

ConstSlice<PermissionOverwriteRecord> chat_state_store_channel_overwrites(
    const ChatStateStore& store, const ChannelRecord& channel) noexcept {
    if (!store.overwrites || !channel.overwrite_count ||
        channel.overwrite_offset + channel.overwrite_count > store.overwrite_count)
        return {};
    return {store.overwrites + channel.overwrite_offset, channel.overwrite_count};
}

} // namespace maicord
