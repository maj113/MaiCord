#include "maicord/model/chat_state.hpp"
#include "maicord/model/permission_bits.hpp"

namespace maicord {
namespace {
constexpr u64 kThreadTestPermissions = kPermissionViewChannel |
                                       kPermissionReadMessageHistory |
                                       kPermissionSendMessagesInThreads |
                                       kPermissionAddReactions | kPermissionAttachFiles;

struct ThreadAuthorityFixture {
    ChatStateStore state{};
    GuildRecord guilds[2]{};
    ChannelRecord channels[8]{};
    RoleRecord roles[4]{};
    PermissionOverwriteRecord overwrites[4]{};
    u16 guild_order[2]{}, channel_order[8]{};
    u8 names[2048]{};

    bool initialize() noexcept {
        if (!chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 8},
                                            {channel_order, 8}, {names, sizeof(names)}) ||
            !chat_state_store_bind_permissions(&state, {roles, 4}, {overwrites, 4}) ||
            !chat_state_store_upsert_guild(
                 &state, {10, utf8_literal("Guild"), GuildMutationKind::create, 99})
                 .ok() ||
            !chat_state_store_upsert_role(&state, {10, 10, kThreadTestPermissions, {}}) ||
            !chat_state_store_set_self_roles(&state, 10, 99, 5, {}))
            return false;
        const ChannelInput inputs[] = {
            {20, 10, 0, utf8_literal("Parent"), 0, 0, 0},
            {21, 10, 0, utf8_literal("Other"), 0, 0, 0},
            {30, 10, 20, utf8_literal("Public"), 0, 0, 11},
            {31, 10, 20, utf8_literal("Private"), 0, 0, 12},
            {32, 10, 21, utf8_literal("Other private"), 0, 0, 12},
            {33, 10, 20, utf8_literal("Announcement"), 0, 0, 10},
        };
        for (const auto& input : inputs)
            if (!chat_state_store_upsert_channel(&state, input).ok())
                return false;
        chat_state_store_recompute_guild_permissions(&state, 10);
        return true;
    }

    ChannelRecord& channel(u64 id) noexcept {
        const auto* record = chat_state_store_find_channel(state, id);
        return channels[record - channels];
    }

    void permissions(u64 value) noexcept {
        chat_state_store_upsert_role(&state, {10, 10, value, {}});
        chat_state_store_recompute_guild_permissions(&state, 10);
    }
};

bool read_batch(const void* context, u32 index, ChannelInput* output) noexcept {
    if (!context || !output)
        return false;
    *output = static_cast<const ChannelInput*>(context)[index];
    return true;
}
} // namespace

void test_thread_authority_model(void (*check)(bool, const char*) noexcept) noexcept {
    ThreadAuthorityFixture fixture{};
    check(fixture.initialize(), "thread authority fixture establishes real parent permissions");
    auto& state = fixture.state;
    auto& public_thread = fixture.channel(30);
    auto& private_thread = fixture.channel(31);
    check(sizeof(ChannelRecord) == 104 && !private_thread.thread_self_joined_known &&
              !chat_state_store_channel_visible(state, private_thread) && private_thread.actions == 0,
          "unknown private membership fails closed without increasing the channel record");
    check(chat_state_store_channel_visible(state, public_thread) &&
              chat_state_store_channel_visible(state, fixture.channel(33)),
          "public and announcement thread reads do not require joining");
    auto& forum_parent = fixture.channel(20);
    forum_parent.type = 15;
    check(!channel_is_message_capable(forum_parent.type) &&
              chat_state_store_channel_visible(state, public_thread) &&
              (chat_state_store_thread_actions(state, public_thread) &
               channel_action_read_history),
          "forum parent visibility authorizes its public post despite not accepting ordinary messages");
    forum_parent.type = 16;
    check(chat_state_store_channel_visible(state, public_thread),
          "media parent visibility also authorizes its public post");
    forum_parent.type = 4;
    check(!chat_state_store_channel_visible(state, public_thread),
          "a category cannot masquerade as a valid thread parent");
    forum_parent.type = 0;

    private_thread.last_message_id = 100;
    private_thread.last_read_message_id = 31;
    private_thread.mention_count = 3;
    const u32 initial_revision = private_thread.revision;
    check(!chat_state_store_set_thread_self_membership(&state, 31, 6, true) &&
              private_thread.revision == initial_revision,
          "membership observation for a different actor makes no mutation");
    check(chat_state_store_set_thread_self_membership(&state, 31, 5, true) &&
              chat_state_store_channel_visible(state, private_thread) &&
              (private_thread.actions & channel_action_send_messages) &&
              fixture.guilds[0].unread_channels == 1 && fixture.guilds[0].mention_count == 3,
          "positive private membership restores actions and guild unread aggregates");
    const u32 joined_revision = private_thread.revision;
    check(chat_state_store_set_thread_self_membership(&state, 31, 5, true) &&
              private_thread.revision == joined_revision,
          "repeated membership proof does not advance publication revisions");
    check(chat_state_store_set_thread_self_membership(&state, 31, 5, false) &&
              private_thread.thread_self_joined_known && !private_thread.thread_self_joined &&
              !chat_state_store_channel_visible(state, private_thread) && !private_thread.actions &&
              !fixture.guilds[0].unread_channels && !fixture.guilds[0].mention_count,
          "explicit self removal revokes private reads and their guild badges");

    fixture.permissions(kThreadTestPermissions | kPermissionManageThreads);
    check(chat_state_store_channel_visible(state, private_thread) && private_thread.actions,
          "Manage Threads permits private reads despite an explicit left membership");
    fixture.permissions((kThreadTestPermissions | kPermissionManageThreads) &
                        ~kPermissionViewChannel);
    check(!chat_state_store_channel_visible(state, private_thread) && !private_thread.actions,
          "Manage Threads cannot bypass missing parent View Channel");
    fixture.permissions(kThreadTestPermissions);
    chat_state_store_set_thread_self_membership(&state, 31, 5, true);

    const u64 raw_permissions = public_thread.effective_permissions;
    check(chat_state_store_set_thread_archived(&state, 30, 5, true) &&
              chat_state_store_channel_visible(state, public_thread) &&
              (public_thread.actions & channel_action_read_history) &&
              !(public_thread.actions &
                (channel_action_send_messages | channel_action_add_reactions)) &&
              public_thread.effective_permissions == raw_permissions,
          "archived thread preserves reads and raw prerequisites while suppressing new writes");
    ChannelInput partial{30, 10, 20, {}, 0, 0, 11};
    check(chat_state_store_upsert_channel(&state, partial).ok() && public_thread.thread_archived &&
              public_thread.thread_archived_known,
          "omitted thread metadata preserves its prior known archive state");
    partial.thread_archived_known = true;
    partial.thread_archived = false;
    partial.thread_locked_known = true;
    partial.thread_locked = true;
    check(chat_state_store_upsert_channel(&state, partial).ok() && !public_thread.thread_archived &&
              public_thread.thread_locked &&
              !(public_thread.actions & channel_action_send_messages) &&
              (public_thread.actions & channel_action_add_reactions),
          "explicit archive false clears it and active locked thread allows reactions but not non-manager "
          "send");
    fixture.permissions(kThreadTestPermissions | kPermissionManageThreads);
    check((public_thread.actions & channel_action_send_messages) != 0,
          "Manage Threads permits writes in an active locked thread");
    fixture.permissions(kThreadTestPermissions);

    chat_state_store_set_thread_self_membership(&state, 32, 5, true);
    chat_state_store_set_thread_archived(&state, 31, 5, false);
    chat_state_store_set_thread_locked(&state, 31, 5, false);
    chat_state_store_invalidate_thread_authority(&state, 10, 20);
    check(!private_thread.thread_self_joined_known && !private_thread.thread_archived_known &&
              !private_thread.thread_locked_known && !private_thread.actions &&
              fixture.channel(32).thread_self_joined_known &&
              chat_state_store_channel_visible(state, public_thread),
          "scoped authority invalidation denies private reads while preserving other parents and public "
          "reads");
    auto* saved_roles = state.roles;
    auto* saved_overwrites = state.overwrites;
    state.roles = nullptr;
    state.overwrites = nullptr;
    check(!chat_state_store_channel_visible(state, private_thread),
          "legacy permission fallback cannot bypass unknown private membership");
    state.roles = saved_roles;
    state.overwrites = saved_overwrites;
    auto& parent = fixture.channel(20);
    const bool parent_resolved = parent.permissions_resolved;
    parent.permissions_resolved = false;
    chat_state_store_set_thread_self_membership(&state, 31, 5, true);
    check(!chat_state_store_channel_visible(state, private_thread),
          "known private membership cannot bypass an unresolved parent permission proof");
    parent.permissions_resolved = parent_resolved;
    const u64 old_parent_id = private_thread.parent_id;
    private_thread.parent_id = 999;
    check(!chat_state_store_channel_visible(state, private_thread) &&
              !chat_state_store_thread_actions(state, private_thread),
          "known private membership cannot bypass a missing parent");
    private_thread.parent_id = old_parent_id;

    chat_state_store_set_thread_self_membership(&state, 31, 5, true);
    partial = {31, 10, 21, {}, 0, 0, 12};
    check(chat_state_store_upsert_channel(&state, partial).ok() &&
              !private_thread.thread_self_joined_known &&
              !chat_state_store_channel_visible(state, private_thread),
          "moving the thread to another parent retires the old membership proof");
    const u16 generation = private_thread.generation;
    check(chat_state_store_remove_channel(&state, 31) &&
              chat_state_store_upsert_channel(&state, partial).ok() &&
              fixture.channel(31).generation != generation && !fixture.channel(31).thread_self_joined_known,
          "recycled thread records cannot retain membership authority");

    const ChannelInput batch[] = {
        {40, 10, 20, utf8_literal("First"), 0, 0, 11},
        {41, 10, 20, utf8_literal("Second"), 0, 0, 11},
        {42, 10, 20, utf8_literal("Third"), 0, 0, 11},
    };
    const u32 before_channels = state.channel_count;
    check(chat_state_store_preflight_channel_batch(state, 3, read_batch, batch) ==
                  ChatStateStoreError::channel_capacity &&
              state.channel_count == before_channels,
          "whole thread sync preflight rejects combined capacity without publishing a prefix");
    const ChannelInput duplicate[] = {batch[0], batch[0]};
    check(chat_state_store_preflight_channel_batch(state, 2, read_batch, duplicate) ==
                  ChatStateStoreError::invalid_input &&
              state.channel_count == before_channels,
          "whole thread sync preflight rejects duplicate identities without mutation");
    check(chat_state_store_preflight_channel_batch(state, 2, read_batch, batch) ==
              ChatStateStoreError::none,
          "whole thread sync preflight admits exact remaining capacity");
    char long_name[400];
    for (char& byte : long_name)
        byte = 'a';
    const ChannelInput names_batch[] = {
        {30, 10, 20, {long_name, sizeof(long_name)}, 0, 0, 11},
        {33, 10, 20, {long_name, sizeof(long_name)}, 0, 0, 10},
        {40, 10, 20, {long_name, sizeof(long_name)}, 0, 0, 11},
    };
    const u32 before_names = state.name_used;
    check(chat_state_store_preflight_channel_batch(state, 3, read_batch, names_batch) ==
                  ChatStateStoreError::name_capacity &&
              state.name_used == before_names && state.channel_count == before_channels,
          "whole thread sync preflight rejects combined name pressure without any partial mutation");
    chat_state_store_set_thread_self_membership(&state, 32, 5, true);
    const u64 before_unavailable_permissions = fixture.channel(32).effective_permissions;
    check(chat_state_store_remove_guild(&state, 10, true) &&
              !fixture.channel(32).thread_self_joined_known && !fixture.channel(32).actions &&
              fixture.channel(32).effective_permissions == before_unavailable_permissions &&
              !chat_state_store_channel_visible(state, public_thread) &&
              !chat_state_store_set_thread_self_membership(&state, 32, 5, true),
          "unavailable guild retires authority and rejects stale observations without erasing raw "
          "permissions");
}
} // namespace maicord
