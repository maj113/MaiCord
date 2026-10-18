#include "maicord/app/navigation_shortcuts.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/ui/navigation_view.hpp"

namespace maicord {
// Conversations holding a mention count are pinned above the servers, newest
// first and at most kNavigationConversationPinMaximum, as in the official rail.
void test_private_rail_pins(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr u32 kChannels = 32;
    ChatStateStore state{};
    GuildRecord guilds[2]{};
    ChannelRecord channels[kChannels]{};
    u16 guild_order[2]{}, channel_order[kChannels]{};
    u8 names[4096]{};
    PrivateChannelStore metadata{};
    check(chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, kChannels},
              {channel_order, kChannels}, {names, sizeof(names)}) &&
              private_channel_store_initialize_owned(&metadata, kChannels, 64),
          "rail pins: state initializes");
    state.private_channels = &metadata;
    state.self_user_id = 1;
    const auto conversation = [&](u64 id, u8 type) noexcept {
        ChannelInput input{};
        input.id = id;
        input.type = type;
        return chat_state_store_upsert_channel(&state, input).ok() &&
               private_channel_store_replace(&metadata, {id, 0, {}, {}, false});
    };
    check(chat_state_store_upsert_guild(&state, {20, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok() &&
              chat_state_store_upsert_channel(&state, {25, 20, 0, utf8_literal("Chat"), 0, 0, 0}).ok() &&
              conversation(40, 1) && conversation(41, 3) && conversation(42, 1) && conversation(43, 1),
          "rail pins: a server and four conversations exist");
    NavigationView view{};
    GuildViewRow guild_rows[24]{};
    ChannelViewRow rows[kChannels]{};
    u16 ordering[kChannels]{};
    u8 visibility[kChannels]{};
    ChannelSortKey keys[kChannels]{};
    check(navigation_view_initialize(&view, {guild_rows, 24}, {rows, kChannels}, {ordering, kChannels},
                                     {visibility, kChannels}, {keys, kChannels}),
          "rail pins: navigation initializes");
    const auto pin = [&](u32 index, u64 id) noexcept {
        return index < view.guild_count && guild_rows[index].id == id &&
               guild_rows[index].folder_role == GuildFolderRole::conversation && guild_rows[index].unread &&
               !guild_view_row_is_guild(guild_rows[index]);
    };
    check(navigation_view_rebuild(&view, state).ok() && view.guild_count == 1 && guild_rows[0].id == 20,
          "rail pins: read conversations are not pinned");
    check(chat_state_store_note_message(&state, 40, 1000, 2, true, false) &&
              chat_state_store_note_message(&state, 41, 2000, 3, true, false) &&
              chat_state_store_note_message(&state, 42, 3000, 2, true, false) &&
              private_channel_store_set_request(&metadata, 42, private_request_pending) &&
              chat_state_store_note_message(&state, 43, 4000, 2, false, false),
          "rail pins: three conversations receive counted messages and one an uncounted one");
    check(navigation_view_rebuild(&view, state).ok() && view.guild_count == 3 && pin(0, 41) && pin(1, 40) &&
              guild_rows[0].mention_count == 1 && guild_rows[0].avatar_shape == 3 &&
              guild_rows[1].avatar_shape == 1 && guild_rows[2].id == 20 &&
              guild_rows[2].folder_role == GuildFolderRole::none,
          "rail pins: counted conversations lead the servers newest first; requests and uncounted ones stay out");
    check(chat_state_store_select_home(&state).ok() && navigation_view_rebuild(&view, state).ok(),
          "rail pins: Home opens");
    const auto next = navigation_target(view, {NavigationMode::guild, false});
    check(next.guild && next.id == 20, "rail pins: server shortcuts skip pinned conversations");
    check(chat_state_store_note_message(&state, 41, 2001, 1, false, false) && navigation_view_rebuild(&view, state).ok() &&
              view.guild_count == 2 && pin(0, 40) && guild_rows[1].id == 20,
          "rail pins: reading a conversation unpins it");
    for (u64 id = 100; id < 125; ++id)
        if (!conversation(id, 1) || !chat_state_store_note_message(&state, id, id * 100, 2, true, false))
            check(false, "rail pins: many conversations receive messages");
    check(navigation_view_rebuild(&view, state).ok() && view.guild_count == kNavigationConversationPinMaximum + 1 &&
              pin(0, 124) && pin(kNavigationConversationPinMaximum - 1, 105) &&
              guild_rows[kNavigationConversationPinMaximum].id == 20,
          "rail pins: at most twenty newest conversations are pinned");
    private_channel_store_release(&metadata);
}
} // namespace maicord
