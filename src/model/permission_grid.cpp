#include "maicord/model/permission_grid.hpp"

namespace maicord {
namespace {
constexpr u8 G = permission_scope_guild, T = permission_scope_text, V = permission_scope_voice,
             S = permission_scope_stage, C = permission_scope_channels;
using PG = PermissionGroup;
#define L(text) utf8_literal(text)
// Label, description, channel label and channel description, NUL-separated.
#define PERMISSION_TEXT(label, description, channel_label, channel_description)                            \
    label "\0" description "\0" channel_label "\0" channel_description
constexpr PermissionGridEntry kEntries[]{
    {kPermissionViewChannel,
     PERMISSION_TEXT("View channels", "See channels and read the messages in them.", "View channel",
                     "See this channel and read its messages."),
     PG::general, G | C},
    {kPermissionManageChannels,
     PERMISSION_TEXT("Manage channels", "Create, edit and delete channels.", "Manage channel",
                     "Rename this channel and change its settings."),
     PG::general, G | C},
    {kPermissionManageRoles,
     PERMISSION_TEXT("Manage roles", "Create and edit roles below their highest role.",
                     "Manage permissions", "Change the overrides on this channel."),
     PG::general, G | C},
    {kPermissionCreateExpressions,
     PERMISSION_TEXT("Create expressions", "Add emoji, stickers and soundboard sounds.", "", ""),
     PG::general, G},
    {kPermissionManageExpressions,
     PERMISSION_TEXT("Manage expressions", "Edit and remove emoji, stickers and sounds.", "", ""),
     PG::general, G},
    {kPermissionViewAuditLog,
     PERMISSION_TEXT("View audit log", "See the record of changes made in this server.", "", ""),
     PG::general, G},
    {kPermissionViewGuildInsights,
     PERMISSION_TEXT("View server insights", "See server activity statistics.", "", ""), PG::general, G},
    {kPermissionManageWebhooks,
     PERMISSION_TEXT("Manage webhooks", "Create, edit and delete webhooks.", "", ""), PG::general,
     G | T | V | S},
    {kPermissionManageGuild,
     PERMISSION_TEXT("Manage server", "Change the server name, system channel and other settings.", "", ""),
     PG::general, G},
    {kPermissionCreateInvite, PERMISSION_TEXT("Create invite", "Invite new people to the server.", "", ""),
     PG::membership, G | C},
    {kPermissionChangeNickname,
     PERMISSION_TEXT("Change nickname", "Change their own nickname in this server.", "", ""),
     PG::membership, G},
    {kPermissionManageNicknames,
     PERMISSION_TEXT("Manage nicknames", "Change other members' nicknames.", "", ""), PG::membership, G},
    {kPermissionKickMembers, PERMISSION_TEXT("Kick members", "Remove members from the server.", "", ""),
     PG::membership, G},
    {kPermissionBanMembers,
     PERMISSION_TEXT("Ban members", "Ban members and delete their recent messages.", "", ""),
     PG::membership, G},
    {kPermissionModerateMembers,
     PERMISSION_TEXT("Time out members", "Stop members from chatting, reacting or speaking for a while.",
                     "", ""),
     PG::membership, G},
    {kPermissionSendMessages,
     PERMISSION_TEXT("Send messages and create posts", "Send messages and start forum posts.", "", ""),
     PG::text, G | C},
    {kPermissionSendMessagesInThreads,
     PERMISSION_TEXT("Send messages in threads", "Reply in threads and forum posts.", "", ""), PG::text,
     G | T},
    {kPermissionCreatePublicThreads,
     PERMISSION_TEXT("Create public threads", "Start threads everyone in the channel can see.", "", ""),
     PG::text, G | T},
    {kPermissionCreatePrivateThreads,
     PERMISSION_TEXT("Create private threads", "Start threads open to invited members only.", "", ""),
     PG::text, G | T},
    {kPermissionEmbedLinks, PERMISSION_TEXT("Embed links", "Show previews for links they post.", "", ""),
     PG::text, G | C},
    {kPermissionAttachFiles, PERMISSION_TEXT("Attach files", "Upload files and media.", "", ""), PG::text,
     G | C},
    {kPermissionAddReactions, PERMISSION_TEXT("Add reactions", "Add new reactions to messages.", "", ""),
     PG::text, G | C},
    {kPermissionUseExternalEmojis,
     PERMISSION_TEXT("Use external emoji", "Use emoji from other servers.", "", ""), PG::text, G | C},
    {kPermissionUseExternalStickers,
     PERMISSION_TEXT("Use external stickers", "Use stickers from other servers.", "", ""), PG::text, G | C},
    {kPermissionMentionEveryone,
     PERMISSION_TEXT("Mention everyone and all roles",
                     "Notify everyone, online members and any role, including unmentionable ones.", "", ""),
     PG::text, G | C},
    {kPermissionManageMessages,
     PERMISSION_TEXT("Manage messages", "Delete messages from others and remove their embeds.", "", ""),
     PG::text, G | C},
    {kPermissionPinMessages, PERMISSION_TEXT("Pin messages", "Pin and unpin messages.", "", ""), PG::text,
     G | C},
    {kPermissionBypassSlowmode,
     PERMISSION_TEXT("Bypass slowmode", "Send messages without waiting for slowmode.", "", ""), PG::text,
     G | C},
    {kPermissionManageThreads,
     PERMISSION_TEXT("Manage threads and posts", "Rename, lock, archive and delete threads and posts.", "",
                     ""),
     PG::text, G | T},
    {kPermissionReadMessageHistory,
     PERMISSION_TEXT("Read message history", "Read messages sent before they joined the channel.", "", ""),
     PG::text, G | C},
    {kPermissionSendSpeechMessages,
     PERMISSION_TEXT("Send text-to-speech messages", "Send messages that are read aloud.", "", ""),
     PG::text, G | C},
    {kPermissionSendVoiceMessages,
     PERMISSION_TEXT("Send voice messages", "Record and send voice clips.", "", ""), PG::text, G | C},
    {kPermissionSendPolls, PERMISSION_TEXT("Create polls", "Start polls in chat.", "", ""), PG::text,
     G | C},
    {kPermissionConnect, PERMISSION_TEXT("Connect", "Join voice channels and hear others.", "", ""),
     PG::voice, G | V | S},
    {kPermissionSpeak, PERMISSION_TEXT("Speak", "Talk in voice channels.", "", ""), PG::voice, G | V},
    {kPermissionStream, PERMISSION_TEXT("Video", "Share their camera or screen.", "", ""), PG::voice,
     G | V},
    {kPermissionUseSoundboard,
     PERMISSION_TEXT("Use soundboard", "Play sounds from the soundboard.", "", ""), PG::voice, G | V},
    {kPermissionUseExternalSounds,
     PERMISSION_TEXT("Use external sounds", "Play soundboard sounds from other servers.", "", ""),
     PG::voice, G | V},
    {kPermissionUseVoiceActivity,
     PERMISSION_TEXT("Use voice activity", "Talk without holding push-to-talk.", "", ""), PG::voice, G | V},
    {kPermissionPrioritySpeaker,
     PERMISSION_TEXT("Priority speaker", "Lower everyone else's volume while they talk.", "", ""),
     PG::voice, G | V},
    {kPermissionMuteMembers, PERMISSION_TEXT("Mute members", "Mute others in voice channels.", "", ""),
     PG::voice, G | V | S},
    {kPermissionDeafenMembers,
     PERMISSION_TEXT("Deafen members", "Deafen others in voice channels.", "", ""), PG::voice, G | V},
    {kPermissionMoveMembers,
     PERMISSION_TEXT("Move members", "Move or disconnect members in voice channels.", "", ""), PG::voice,
     G | V | S},
    {kPermissionSetVoiceChannelStatus,
     PERMISSION_TEXT("Set voice channel status", "Change the status shown on a voice channel.", "", ""),
     PG::voice, G | V},
    {kPermissionUseApplicationCommands,
     PERMISSION_TEXT("Use application commands", "Use slash commands and app menus.", "", ""), PG::apps,
     G | C},
    {kPermissionUseActivities, PERMISSION_TEXT("Use activities", "Start and join activities.", "", ""),
     PG::apps, G | T | V},
    {kPermissionUseExternalApps,
     PERMISSION_TEXT("Use external apps", "Let apps they added post public replies.", "", ""), PG::apps,
     G | C},
    {kPermissionRequestToSpeak,
     PERMISSION_TEXT("Request to speak", "Ask to speak in stage channels.", "", ""), PG::stage, G | S},
    {kPermissionCreateEvents, PERMISSION_TEXT("Create events", "Schedule events.", "", ""), PG::events,
     G | V | S},
    {kPermissionManageEvents, PERMISSION_TEXT("Manage events", "Edit and cancel any event.", "", ""),
     PG::events, G | V | S},
    {kPermissionAdministrator,
     PERMISSION_TEXT(
         "Administrator",
         "Grants every permission and ignores channel overrides. Give it only to people you trust.", "",
         ""),
     PG::advanced, G},
};
#undef PERMISSION_TEXT
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

namespace {
// Field index of the entry text: the guild wording, or the channel wording
// when an override scope renames it.
Utf8View permission_grid_text(const PermissionGridEntry& entry, u8 scope, u32 index) noexcept {
    Utf8View fields[4];
    const char* cursor = entry.text;
    for (auto& field : fields) {
        u32 bytes = 0;
        while (cursor[bytes])
            ++bytes;
        field = {cursor, bytes};
        cursor += bytes + 1;
    }
    return !(scope & permission_scope_guild) && fields[index + 2].bytes ? fields[index + 2] : fields[index];
}
} // namespace

Utf8View permission_grid_label(const PermissionGridEntry& entry, u8 scope) noexcept {
    return permission_grid_text(entry, scope, 0);
}

Utf8View permission_grid_description(const PermissionGridEntry& entry, u8 scope) noexcept {
    return permission_grid_text(entry, scope, 1);
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
