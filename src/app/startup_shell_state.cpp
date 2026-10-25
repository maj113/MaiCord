#include "maicord/app/startup_shell.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/guild_display_order.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/user_store.hpp"

namespace maicord {
namespace {

// Newest resident messages whose authors are recorded: the startup page and then some.
constexpr u32 kAuthorMessages = 50;

Utf8View text(const StartupShell& shell, StartupShellText value) noexcept {
    return startup_shell_text(shell, value);
}

bool has_user(const StartupShell& shell, u64 id) noexcept {
    for (u32 i = 0; i < shell.user_count; ++i)
        if (shell.users[i].id == id)
            return true;
    return false;
}

void add_user(StartupShell* shell, const UserStore& users, u64 id) noexcept {
    const UserRecord* user = id ? user_store_find(users, id) : nullptr;
    if (!user || has_user(*shell, id) || shell->user_count >= kStartupShellUserCapacity)
        return;
    StartupShellUser& out = shell->users[shell->user_count];
    out.id = id;
    if (startup_shell_add_text(shell, user_store_name(users, *user), &out.name) &&
        startup_shell_add_text(shell, user_store_username(users, *user), &out.username) &&
        startup_shell_add_text(shell, user_store_avatar_hash(*user), &out.avatar) && out.name.bytes)
        ++shell->user_count;
}

bool thread_shown(const ChannelRecord& channel) noexcept {
    return channel.thread_self_joined_known && channel.thread_self_joined &&
           !(channel.thread_archived_known && channel.thread_archived);
}

void add_channel(StartupShell* shell, const ChatStateStore& state, const ChannelRecord& channel) noexcept {
    for (u32 i = 0; i < shell->channel_count; ++i)
        if (shell->channels[i].id == channel.id)
            return;
    if (shell->channel_count >= kStartupShellChannelCapacity)
        return;
    StartupShellChannel& out = shell->channels[shell->channel_count];
    out.id = channel.id;
    out.parent_id = channel.parent_id;
    out.permissions = channel.effective_permissions;
    out.position = channel.position;
    out.flags = channel.flags;
    out.type = channel.type;
    out.state = static_cast<u8>(
        (channel.collapsed ? startup_shell_channel_collapsed : 0) |
        (channel.notification_muted ? startup_shell_channel_muted : 0) |
        (channel.notification_dimmed ? startup_shell_channel_dimmed : 0) |
        (channel.opted_in ? startup_shell_channel_opted_in : 0) |
        (channel.thread_self_joined_known && channel.thread_self_joined ? startup_shell_thread_joined : 0) |
        (channel.thread_archived_known && channel.thread_archived ? startup_shell_thread_archived : 0) |
        (channel.thread_locked_known && channel.thread_locked ? startup_shell_thread_locked : 0));
    if (startup_shell_add_text(shell, chat_state_store_channel_name(state, channel), &out.name))
        ++shell->channel_count;
}

void capture_channels(StartupShell* shell, const ChatStateStore& state) noexcept {
    // The selected channel and its parents go first, so a full table keeps them.
    u64 pinned[3]{shell->channel_id};
    for (u32 i = 1; i < 3; ++i) {
        const ChannelRecord* parent = pinned[i - 1] ? chat_state_store_find_channel(state, pinned[i - 1]) : nullptr;
        pinned[i] = parent ? parent->parent_id : 0;
    }
    for (const u64 id : pinned)
        if (const ChannelRecord* channel = id ? chat_state_store_find_channel(state, id) : nullptr;
            channel && channel->guild_id == shell->guild_id && chat_state_store_channel_visible(state, *channel))
            add_channel(shell, state, *channel);
    // Channels before threads: a full table drops threads first.
    for (u32 pass = 0; pass < 2; ++pass)
        for (u32 order = 0; order < state.channel_count; ++order) {
            const ChannelRecord& channel = state.channels[state.channel_order[order]];
            const bool thread = channel_is_thread(channel.type);
            if (thread != (pass == 1) || channel.guild_id != shell->guild_id ||
                !chat_state_store_channel_visible(state, channel) || (thread && !thread_shown(channel)))
                continue;
            add_channel(shell, state, channel);
        }
}

u64 conversation_recency(const ChatStateStore& state, const ChannelRecord& channel) noexcept {
    const auto* metadata =
        state.private_channels ? private_channel_store_find(*state.private_channels, channel.id) : nullptr;
    // Recency in the high bits, the store's touch order below.
    return (private_channel_recency(channel.id, channel.last_message_id) << 20) |
           ((metadata ? metadata->sort_sequence : 0) & 0xFFFFF);
}

void capture_conversations(StartupShell* shell, const StartupShellStores& stores) noexcept {
    const ChatStateStore& state = *stores.state;
    if (!state.private_channels)
        return;
    // The newest conversations, newest first.
    const ChannelRecord* newest[kStartupShellConversationCapacity];
    u64 keys[kStartupShellConversationCapacity];
    u32 count = 0;
    for (u32 order = 0; order < state.channel_count; ++order) {
        const ChannelRecord& channel = state.channels[state.channel_order[order]];
        if (!channel_is_private(channel.type) || !chat_state_store_channel_visible(state, channel))
            continue;
        const u64 key = conversation_recency(state, channel);
        u32 at = count;
        while (at && keys[at - 1] < key)
            --at;
        if (at >= kStartupShellConversationCapacity)
            continue;
        const u32 last = minimum(count, kStartupShellConversationCapacity - 1);
        for (u32 i = last; i > at; --i) {
            newest[i] = newest[i - 1];
            keys[i] = keys[i - 1];
        }
        newest[at] = &channel;
        keys[at] = key;
        count = minimum(count + 1, kStartupShellConversationCapacity);
    }
    for (u32 i = 0; i < count; ++i) {
        const ChannelRecord& channel = *newest[i];
        const PrivateChannelRecord* metadata = private_channel_store_find(*state.private_channels, channel.id);
        if (!metadata)
            continue;
        StartupShellConversation& out = shell->conversations[shell->conversation_count];
        out.id = channel.id;
        out.owner_id = metadata->owner_id;
        out.last_message_id = channel.last_message_id;
        out.type = channel.type;
        out.first_recipient = shell->recipient_count;
        out.recipient_count = 0;
        const ConstSlice<u64> recipients = private_channel_store_recipients(*state.private_channels, *metadata);
        for (u32 r = 0; r < recipients.count && out.recipient_count < kStartupShellConversationRecipients &&
                        shell->recipient_count < kStartupShellRecipientCapacity;
             ++r) {
            if (!recipients[r])
                continue;
            shell->recipients[shell->recipient_count++] = recipients[r];
            ++out.recipient_count;
            add_user(shell, *stores.users, recipients[r]);
        }
        if (startup_shell_add_text(shell, chat_state_store_channel_name(state, channel), &out.name) &&
            startup_shell_add_text(shell, {metadata->icon, metadata->icon_bytes}, &out.icon))
            ++shell->conversation_count;
        else
            shell->recipient_count = out.first_recipient;
    }
}

void capture_authors(StartupShell* shell, const StartupShellStores& stores, const MessageStore& messages) noexcept {
    u32 seen = 0;
    for (u32 index = messages.count; index != 0 && seen < kAuthorMessages; --index) {
        const MessageRecord* record = message_store_resolve(messages, message_store_ordered_handle(messages, index - 1));
        if (!record || record->channel_id != shell->channel_id || !record->author_id)
            continue;
        ++seen;
        add_user(shell, *stores.users, record->author_id);
        if (!shell->guild_id || !stores.members || shell->author_count >= kStartupShellAuthorCapacity)
            continue;
        bool known = false;
        for (u32 i = 0; i < shell->author_count && !known; ++i)
            known = shell->authors[i].user_id == record->author_id;
        const GuildMemberRecord* member =
            known ? nullptr : guild_member_store_find(*stores.members, shell->guild_id, record->author_id);
        if (!member)
            continue;
        StartupShellAuthor& out = shell->authors[shell->author_count];
        out.user_id = record->author_id;
        out.role_color = member->role_color;
        if (startup_shell_add_text(shell, guild_member_store_nickname(*member), &out.nickname))
            ++shell->author_count;
    }
}

} // namespace

void startup_shell_capture(const StartupShellStores& stores, const MessageStore* messages,
                           StartupShell* shell) noexcept {
    if (!shell || !stores.state || !stores.users)
        return;
    StartupShell& s = *shell;
    const ChatStateStore& state = *stores.state;
    s.self_user_id = state.self_user_id;
    s.guild_id = state.home_selected ? 0 : state.selected_guild_id;
    s.channel_id = state.selected_channel_id;
    s.guild_count = s.channel_count = s.conversation_count = s.recipient_count = 0;
    s.user_count = s.author_count = s.text_bytes = 0;
    add_user(shell, *stores.users, state.self_user_id);
    if (const auto* status = user_appearance_find(stores.users->appearance, state.self_user_id);
        status && (status->fields & appearance_activity) && (status->fields & appearance_custom_status)) {
        s.status_emoji_id = status->status_emoji_id;
        if (!startup_shell_add_text(shell, status->activity.view(), &s.status_text) ||
            !startup_shell_add_text(shell, status->status_emoji.view(), &s.status_emoji))
            s.status_text = s.status_emoji = {};
    }
    // Rail order, so a full table keeps the rows on screen.
    constexpr u32 kRail = 1024;
    GuildDisplayEntry rail[kRail];
    const u32 rows = guild_display_order_build(stores.folders, state, {rail, kRail});
    for (u32 row = 0; row < rows && s.guild_count < kStartupShellGuildCapacity; ++row) {
        const GuildRecord* found = chat_state_store_find_guild(state, rail[row].guild_id);
        if (!found)
            continue;
        const GuildRecord& guild = *found;
        StartupShellGuild& out = s.guilds[s.guild_count];
        out.id = guild.id;
        out.flags = static_cast<u8>((guild.unavailable ? startup_shell_guild_unavailable : 0) |
                                    (guild.notification_muted ? startup_shell_guild_muted : 0) |
                                    (guild.hide_muted_channels ? startup_shell_guild_hide_muted : 0) |
                                    (guild.channel_opt_in ? startup_shell_guild_opt_in : 0));
        out.banner = {};
        if (startup_shell_add_text(shell, chat_state_store_guild_name(state, guild), &out.name) &&
            startup_shell_add_text(shell, chat_state_store_guild_icon_hash(guild), &out.icon) &&
            (guild.id != s.guild_id ||
             startup_shell_add_text(shell, chat_state_store_guild_banner_hash(guild), &out.banner)))
            ++s.guild_count;
    }
    if (s.guild_id)
        capture_channels(shell, state);
    else
        capture_conversations(shell, stores);
    if (messages && s.channel_id)
        capture_authors(shell, stores, *messages);
}

bool startup_shell_apply(const StartupShell& s, const StartupShellStores& stores) noexcept {
    if (!stores.state || !stores.users || !s.self_user_id)
        return false;
    ChatStateStore& state = *stores.state;
    // Each upsert would select the restoration target as soon as it is
    // resident; the saved selection is placed once everything is in.
    const u64 restore_channel = state.restore_channel_id, restore_guild = state.restore_guild_id;
    state.restore_channel_id = state.restore_guild_id = 0;
    state.self_user_id = s.self_user_id;
    for (u32 i = 0; i < s.guild_count; ++i) {
        const StartupShellGuild& g = s.guilds[i];
        GuildInput input{g.id, text(s, g.name),
                         (g.flags & startup_shell_guild_unavailable) ? GuildMutationKind::unavailable
                                                                     : GuildMutationKind::create,
                         0};
        input.icon_hash = text(s, g.icon);
        input.icon_known = true;
        input.banner_hash = text(s, g.banner);
        input.banner_known = true;
        if (!chat_state_store_upsert_guild(&state, input).ok())
            continue;
        chat_state_store_set_guild_mute(&state, g.id, (g.flags & startup_shell_guild_muted) != 0);
        chat_state_store_set_guild_hide_muted(&state, g.id, (g.flags & startup_shell_guild_hide_muted) != 0);
        chat_state_store_set_guild_channel_opt_in(&state, g.id, (g.flags & startup_shell_guild_opt_in) != 0);
    }
    // Threads inherit their parents' permissions, so parents go first.
    for (u32 pass = 0; pass < 2 && s.guild_id; ++pass)
        for (u32 i = 0; i < s.channel_count; ++i) {
            const StartupShellChannel& c = s.channels[i];
            const bool thread = channel_is_thread(c.type);
            if (thread != (pass == 1))
                continue;
            ChannelInput input{c.id, s.guild_id, c.parent_id, text(s, c.name), c.position, c.flags, c.type};
            input.name_known = true;
            if (thread) {
                input.thread_self_joined = (c.state & startup_shell_thread_joined) != 0;
                input.thread_archived = (c.state & startup_shell_thread_archived) != 0;
                input.thread_locked = (c.state & startup_shell_thread_locked) != 0;
                input.thread_self_joined_known = input.thread_archived_known = input.thread_locked_known = true;
            }
            if (!chat_state_store_upsert_channel(&state, input).ok() ||
                !chat_state_store_restore_channel(&state, c.id, c.permissions, 0))
                continue;
            chat_state_store_set_channel_mute(&state, c.id, (c.state & startup_shell_channel_muted) != 0,
                                              (c.state & startup_shell_channel_dimmed) != 0);
            chat_state_store_set_channel_opted_in(&state, c.id, (c.state & startup_shell_channel_opted_in) != 0);
            if (c.type == 4 && (c.state & startup_shell_channel_collapsed))
                state.channels[chat_state_store_find_channel(state, c.id) - state.channels].collapsed = true;
        }
    for (u32 i = 0; i < s.conversation_count && state.private_channels; ++i) {
        const StartupShellConversation& c = s.conversations[i];
        ChannelInput input{c.id, 0, 0, text(s, c.name), 0, 0, c.type};
        input.name_known = true;
        PrivateChannelInput metadata{};
        metadata.id = c.id;
        metadata.owner_id = c.owner_id;
        metadata.recipients = {s.recipients + c.first_recipient, c.recipient_count};
        metadata.icon = text(s, c.icon);
        metadata.recipients_known = metadata.owner_known = metadata.icon_known = true;
        if (!private_channel_store_prepare(state.private_channels, metadata) ||
            !chat_state_store_upsert_channel(&state, input).ok())
            continue;
        private_channel_store_replace(state.private_channels, metadata);
        chat_state_store_restore_channel(&state, c.id, 0, c.last_message_id);
    }
    for (u32 i = 0; i < s.user_count; ++i) {
        const StartupShellUser& u = s.users[i];
        if (user_store_upsert(stores.users, u.id, text(s, u.name), text(s, u.username)).ok())
            user_store_set_avatar(stores.users, u.id, text(s, u.avatar));
    }
    for (u32 i = 0; i < s.author_count && s.guild_id && stores.members; ++i)
        guild_member_store_restore_display(stores.members, state, s.guild_id, s.authors[i].user_id,
                                           text(s, s.authors[i].nickname), s.authors[i].role_color);
    if (s.status_text.bytes || s.status_emoji.bytes) {
        UserAppearance status{};
        status.user_id = s.self_user_id;
        status.fields = appearance_activity | appearance_custom_status;
        status.status_emoji_id = s.status_emoji_id;
        const Utf8View line = text(s, s.status_text), emoji = text(s, s.status_emoji);
        status.activity.bytes = static_cast<u16>(minimum<u32>(line.bytes, sizeof(status.activity.data)));
        copy_bytes(status.activity.data, line.data, status.activity.bytes);
        status.status_emoji.bytes = static_cast<u16>(minimum<u32>(emoji.bytes, sizeof(status.status_emoji.data)));
        copy_bytes(status.status_emoji.data, emoji.data, status.status_emoji.bytes);
        if (user_appearance_upsert(&stores.users->appearance, status))
            saturating_increment(stores.users->revision);
    }
    const bool placed =
        (s.channel_id && chat_state_store_select_channel(&state, s.channel_id).ok()) ||
        (s.guild_id && chat_state_store_select_pending_guild(&state, s.guild_id, s.channel_id).ok());
    if (!placed) {
        state.restore_channel_id = restore_channel;
        state.restore_guild_id = restore_guild;
    }
    return placed;
}

} // namespace maicord
