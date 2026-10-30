#include "maicord/core/time.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/model/read_state.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/permission_bits.hpp"
#include "benchmark_output.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kMessageItems = 8'192;
inline constexpr u32 kLookupIterations = 5'000'000;
inline constexpr u32 kOrderedIterations = 5'000'000;
inline constexpr u32 kUpdateIterations = 1'000'000;
inline constexpr u32 kTextCapacity = 256 * 1024;
inline constexpr u32 kStateGuilds = 256;
inline constexpr u32 kStateChannels = 4'096;
inline constexpr u32 kStateNames = 256 * 1024;
inline constexpr u32 kStateLookupIterations = 5'000'000;
inline constexpr u32 kStateUpdateIterations = 1'000'000;
inline constexpr u32 kPermissionChannels = 256;
inline constexpr u32 kPermissionRoles = 16;
inline constexpr u32 kPermissionOverwrites = kPermissionChannels * 2;
inline constexpr u32 kPermissionIterations = 20'000;
inline constexpr u32 kActivityIterations = 2'000'000;
inline constexpr u32 kRichItems = 256;
inline constexpr u32 kRichTextCapacity = 256 * 1024;
inline constexpr u32 kRichUpdateIterations = 500'000;
inline constexpr u32 kRichReactionIterations = 1'000'000;
inline constexpr u32 kUserCount = 256;
inline constexpr u32 kUserUpsertIterations = 2'000'000;
inline constexpr u32 kMemberCount = 400;
inline constexpr u32 kMemberIterations = 200'000;
inline constexpr u32 kRichLookupIterations = 5'000'000;

MessageRecord g_records[kMessageItems];
u32 g_order[kMessageItems];
u8 g_text[kTextCapacity];
GuildRecord g_guilds[kStateGuilds];
ChannelRecord g_channels[kStateChannels];
u16 g_guild_order[kStateGuilds];
u16 g_channel_order[kStateChannels];
u8 g_state_names[kStateNames];
GuildRecord g_permission_guilds[1];
ChannelRecord g_permission_channels[kPermissionChannels];
RoleRecord g_permission_roles[kPermissionRoles];
PermissionOverwriteRecord g_permission_overwrites[kPermissionOverwrites];
u16 g_permission_guild_order[1];
u16 g_permission_channel_order[kPermissionChannels];
u8 g_permission_names[32 * 1024];
MessageRichRecord g_rich_records[kRichItems];
u16 g_rich_order[kRichItems];
u8 g_rich_text[kRichTextCapacity];
UserRecord g_users[kUserCount];
u32 g_user_order[kUserCount];
u8 g_user_names[32 * 1024];
GuildMemberRecord g_members[512];
u32 g_member_order[512];
volatile u64 g_model_checksum = 0;

int run() noexcept {
    MessageStore store{};
    if (!message_store_initialize(&store, Slice<MessageRecord>{g_records, kMessageItems},
                                  Slice<u32>{g_order, kMessageItems}, Slice<u8>{g_text, kTextCapacity})) {
        return 1;
    }
    constexpr u64 first_id = 1'270'000'000'000'000'000ull;
    constexpr u64 channel_id = 1'260'000'000'000'000'001ull;
    constexpr Utf8View content = utf8_literal("stable benchmark message");
    const Clock clock = clock_initialize();
    i64 begin = clock_now();
    for (u32 index = 0; index < kMessageItems; ++index) {
        const MessageInput input{
            first_id + static_cast<u64>(index) * 4'194'304ull,
            channel_id,
            1'250'000'000'000'000'001ull + (index & 63),
            0,
            0,
            content,
            index & 7,
        };
        if (!message_store_upsert(&store, input).ok()) {
            return 2;
        }
    }
    const u64 append_us = clock_microseconds(clock, begin, clock_now());

    u64 checksum = 0;
    begin = clock_now();
    for (u32 iteration = 0; iteration < kLookupIterations; ++iteration) {
        const u32 index = (iteration * 4'053u) & (kMessageItems - 1);
        const MessageStoreResult found =
            message_store_find(store, first_id + static_cast<u64>(index) * 4'194'304ull);
        checksum += found.handle.slot + found.order_index + found.handle.generation;
    }
    const u64 lookup_us = clock_microseconds(clock, begin, clock_now());

    begin = clock_now();
    for (u32 iteration = 0; iteration < kOrderedIterations; ++iteration) {
        const u32 index = (iteration * 2'981u) & (kMessageItems - 1);
        const MessageHandle handle = message_store_ordered_handle(store, index);
        const MessageRecord* record = message_store_resolve(store, handle);
        if (!record) {
            return 3;
        }
        checksum += record->id + record->content_bytes;
    }
    const u64 ordered_us = clock_microseconds(clock, begin, clock_now());

    begin = clock_now();
    for (u32 iteration = 0; iteration < kUpdateIterations; ++iteration) {
        const u32 index = (iteration * 1'747u) & (kMessageItems - 1);
        const MessageInput input{
            first_id + static_cast<u64>(index) * 4'194'304ull,
            channel_id,
            1'250'000'000'000'000'001ull + (index & 63),
            0,
            iteration,
            content,
            index & 7,
        };
        const MessageStoreResult result = message_store_upsert(&store, input);
        if (!result.ok() || result.inserted) {
            return 4;
        }
        checksum += result.handle.slot + result.order_index;
    }
    const u64 update_us = clock_microseconds(clock, begin, clock_now());

    ChatStateStore state{};
    if (!chat_state_store_initialize(&state, Slice<GuildRecord>{g_guilds, kStateGuilds},
                                        Slice<u16>{g_guild_order, kStateGuilds},
                                        Slice<ChannelRecord>{g_channels, kStateChannels},
                                        Slice<u16>{g_channel_order, kStateChannels},
                                        Slice<u8>{g_state_names, kStateNames})) {
        return 5;
    }
    constexpr u64 first_guild_id = 1'100'000'000'000'000'000ull;
    constexpr u64 first_channel_id = 1'200'000'000'000'000'000ull;
    begin = clock_now();
    for (u32 guild = 0; guild < kStateGuilds; ++guild) {
        if (!chat_state_store_upsert_guild(&state, GuildInput{first_guild_id + guild,
                                                                        utf8_literal("benchmark guild"),
                                                                        GuildMutationKind::create})
                 .ok()) {
            return 6;
        }
    }
    for (u32 channel = 0; channel < kStateChannels; ++channel) {
        if (!chat_state_store_upsert_channel(
                 &state,
                 ChannelInput{first_channel_id + channel, first_guild_id + channel % kStateGuilds, 0,
                                     utf8_literal("benchmark channel"), channel & 31u, 0, 0})
                 .ok()) {
            return 7;
        }
    }
    const u64 state_append_us = clock_microseconds(clock, begin, clock_now());

    begin = clock_now();
    for (u32 iteration = 0; iteration < kStateLookupIterations; ++iteration) {
        const u32 index = (iteration * 2'981u) & (kStateChannels - 1);
        const ChannelRecord* record =
            chat_state_store_find_channel(state, first_channel_id + index);
        if (!record) {
            return 8;
        }
        checksum += record->id + record->position;
    }
    const u64 state_lookup_us = clock_microseconds(clock, begin, clock_now());

    begin = clock_now();
    for (u32 iteration = 0; iteration < kStateUpdateIterations; ++iteration) {
        const u32 index = (iteration * 1'747u) & (kStateChannels - 1);
        const ChatStateStoreResult updated = chat_state_store_upsert_channel(
            &state, ChannelInput{first_channel_id + index, first_guild_id + index % kStateGuilds, 0,
                                        utf8_literal("benchmark channel"), index & 31u, iteration & 7u, 0});
        if (!updated.ok() || updated.inserted) {
            return 9;
        }
        checksum += updated.order_index;
    }
    const u64 state_update_us = clock_microseconds(clock, begin, clock_now());

    ChatStateStore permission_state{};
    if (!chat_state_store_initialize(
            &permission_state, Slice<GuildRecord>{g_permission_guilds, 1},
            Slice<u16>{g_permission_guild_order, 1},
            Slice<ChannelRecord>{g_permission_channels, kPermissionChannels},
            Slice<u16>{g_permission_channel_order, kPermissionChannels},
            Slice<u8>{g_permission_names, static_cast<u32>(sizeof(g_permission_names))}) ||
        !chat_state_store_bind_permissions(
            &permission_state, Slice<RoleRecord>{g_permission_roles, kPermissionRoles},
            Slice<PermissionOverwriteRecord>{g_permission_overwrites,
                                                    kPermissionOverwrites}) ||
        !chat_state_store_upsert_guild(
             &permission_state,
             GuildInput{500, utf8_literal("permissions"), GuildMutationKind::create, 999})
             .ok() ||
        !chat_state_store_upsert_role(
            &permission_state,
            RoleInput{500, 500, kPermissionViewChannel})) {
        return 10;
    }
    u64 self_roles[kPermissionRoles - 1]{};
    for (u32 index = 1; index < kPermissionRoles; ++index) {
        self_roles[index - 1] = 700 + index;
        if (!chat_state_store_upsert_role(
                &permission_state, RoleInput{700 + index, 500, 0})) {
            return 10;
        }
    }
    for (u32 index = 0; index < kPermissionChannels; ++index) {
        const u64 permission_channel_id = 6'000 + index;
        if (!chat_state_store_upsert_channel(
                 &permission_state,
                 ChannelInput{permission_channel_id, 500, 0,
                                     utf8_literal("permission channel"), index, 0,
                                     0})
                 .ok() ||
            !chat_state_store_append_channel_overwrite(
                &permission_state, permission_channel_id,
                PermissionOverwriteInput{500, 0, kPermissionViewChannel, 0}) ||
            !chat_state_store_append_channel_overwrite(
                &permission_state, permission_channel_id,
                PermissionOverwriteInput{701, kPermissionViewChannel, 0, 0})) {
            return 10;
        }
    }
    if (!chat_state_store_set_self_roles(
            &permission_state, 500, 999, 1,
            ConstSlice<u64>{self_roles, kPermissionRoles - 1})) {
        return 10;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kPermissionIterations; ++iteration) {
        if (!chat_state_store_upsert_role(
                &permission_state,
                RoleInput{701, 500,
                                 (iteration & 1u) != 0 ? kPermissionAdministrator : 0})) {
            return 10;
        }
        chat_state_store_recompute_guild_permissions(&permission_state, 500);
        checksum += permission_state.permission_mutations + permission_state.selected_channel_id;
    }
    const u64 permission_us = clock_microseconds(clock, begin, clock_now());

    u64 activity_operations = 0;
    begin = clock_now();
    for (u32 iteration = 0; iteration < kActivityIterations; ++iteration) {
        const u64 activity_channel_id = 6'000 + (iteration & (kPermissionChannels - 1));
        if (!chat_state_store_note_message(
                &permission_state, activity_channel_id,
                1'300'000'000'000'000'000ull + iteration, 2,
                (iteration & 15u) == 0, false)) {
            return 11;
        }
        ++activity_operations;
        if ((iteration & 7u) == 7u) {
            if (!read_state_mark_local(&permission_state, activity_channel_id,
                                       1'300'000'000'000'000'000ull + iteration, 0, 0)) {
                return 11;
            }
            ++activity_operations;
        }
        checksum += permission_state.activity_mutations +
                    g_permission_guilds[0].mention_count;
    }
    const u64 activity_us = clock_microseconds(clock, begin, clock_now());

    MessageRichStore rich_store{};
    if (!message_rich_store_initialize(
            &rich_store,
            Slice<MessageRichRecord>{g_rich_records, kRichItems},
            Slice<u16>{g_rich_order, kRichItems},
            Slice<u8>{g_rich_text, kRichTextCapacity})) {
        return 12;
    }
    GatewayMessageReaction rich_reactions[1]{};
    rich_reactions[0].emoji_id = 44;
    rich_reactions[0].name = utf8_literal("party");
    rich_reactions[0].count = 7;
    rich_reactions[0].normal_count = 7;
    rich_reactions[0].me = true;
    GatewayMessageAttachment rich_attachments[1]{};
    rich_attachments[0].id = 70;
    rich_attachments[0].size = 12'345;
    rich_attachments[0].filename = utf8_literal("clip.mp4");
    rich_attachments[0].content_type = utf8_literal("video/mp4");
    rich_attachments[0].url = utf8_literal(
        "https://cdn.discordapp.com/attachments/7/70/clip.mp4");
    GatewayMessageEmbed rich_embeds[1]{};
    rich_embeds[0].title = utf8_literal("Build");
    rich_embeds[0].description = utf8_literal("green");
    rich_embeds[0].author_name = utf8_literal("CI");
    rich_embeds[0].footer_text = utf8_literal("done");
    rich_embeds[0].type = GatewayMessageEmbedType::rich;
    rich_embeds[0].field_count = 1;
    GatewayMessageEmbedField rich_fields[1]{};
    rich_fields[0].name = utf8_literal("Target");
    rich_fields[0].value = utf8_literal("Release");
    GatewayMessageSticker rich_stickers[1]{};
    rich_stickers[0].id = 88;
    rich_stickers[0].name = utf8_literal("Wave");
    rich_stickers[0].format_type = 2;
    GatewayMessageComponent rich_components[2]{};
    rich_components[0].type = 10;
    rich_components[0].parent = 0xFFFFu;
    rich_components[0].content = utf8_literal("Component markdown");
    rich_components[1].type = 2;
    rich_components[1].parent = 0xFFFFu;
    rich_components[1].label = utf8_literal("Native button");
    GatewayMessageEvent rich_message{};
    rich_message.id = first_id;
    rich_message.fields = gateway_message_field_message_reference |
                          gateway_message_field_referenced_message |
                          gateway_message_field_reactions |
                          gateway_message_field_attachments |
                          gateway_message_field_embeds |
                          gateway_message_field_sticker_items |
                          gateway_message_field_components;
    rich_message.reference.message_id = first_id - 1;
    rich_message.reference.channel_id = channel_id;
    rich_message.reference.author_id = 55;
    rich_message.reference.author_name = utf8_literal("Source");
    rich_message.reference.content = utf8_literal("Referenced content");
    rich_message.reference.state = GatewayMessageReferenceState::available;
    rich_message.reactions =
        ConstSlice<GatewayMessageReaction>{rich_reactions, 1};
    rich_message.attachments =
        ConstSlice<GatewayMessageAttachment>{rich_attachments, 1};
    rich_message.embeds = ConstSlice<GatewayMessageEmbed>{rich_embeds, 1};
    rich_message.embed_fields =
        ConstSlice<GatewayMessageEmbedField>{rich_fields, 1};
    rich_message.stickers =
        ConstSlice<GatewayMessageSticker>{rich_stickers, 1};
    rich_message.components =
        ConstSlice<GatewayMessageComponent>{rich_components, 2};
    if (!message_rich_store_apply(&rich_store, first_id, 1, rich_message)
             .ok()) {
        return 12;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kRichUpdateIterations; ++iteration) {
        rich_reactions[0].count = 1 + (iteration & 15u);
        rich_reactions[0].normal_count = rich_reactions[0].count;
        const MessageRichStoreResult updated = message_rich_store_apply(
            &rich_store, first_id, iteration + 2, rich_message);
        if (!updated.ok() || !updated.record) {
            return 12;
        }
        checksum += updated.record->reactions[0].count +
                    updated.record->component_count;
    }
    const u64 rich_update_us =
        clock_microseconds(clock, begin, clock_now());
    constexpr u32 kRichResident = 64;
    for (u32 index = 1; index < kRichResident; ++index) {
        rich_message.id = first_id + index;
        if (!message_rich_store_apply(&rich_store, first_id + index, 1, rich_message).ok()) {
            return 12;
        }
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kRichLookupIterations; ++iteration) {
        const MessageRichRecord* rich =
            message_rich_store_find(rich_store, first_id + ((iteration * 37u) & (kRichResident - 1)));
        if (!rich) {
            return 12;
        }
        checksum += rich->message_revision + rich->reaction_count;
    }
    const u64 rich_lookup_us =
        clock_microseconds(clock, begin, clock_now());

    // The update loop left the record at revision kRichUpdateIterations + 1.
    const MessageRichReactionMutation reaction_mutations[4] = {
        {44, utf8_literal("party"), MessageReactionType::normal, MessageRichReactionMutationKind::add, false, false},
        {44, utf8_literal("party"), MessageReactionType::normal, MessageRichReactionMutationKind::remove, false, false},
        {0, utf8_literal("\xF0\x9F\x8E\x89"), MessageReactionType::normal, MessageRichReactionMutationKind::add,
         false, false},
        {0, utf8_literal("\xF0\x9F\x8E\x89"), MessageReactionType::normal, MessageRichReactionMutationKind::remove,
         false, false},
    };
    begin = clock_now();
    for (u32 iteration = 0; iteration < kRichReactionIterations; ++iteration) {
        const MessageRichStoreResult mutated = message_rich_store_mutate_reaction(
            &rich_store, first_id, kRichUpdateIterations + 1, reaction_mutations[iteration & 3u]);
        if (!mutated.ok() || !mutated.record) {
            return 12;
        }
        checksum += mutated.record->reaction_count;
    }
    const u64 rich_reaction_us = clock_microseconds(clock, begin, clock_now());
    UserStore users{};
    if (!user_store_initialize(&users, Slice<UserRecord>{g_users, kUserCount}, Slice<u32>{g_user_order, kUserCount},
                               Slice<u8>{g_user_names, sizeof(g_user_names)})) {
        return 14;
    }
    for (u32 index = 0; index < kUserCount; ++index) {
        if (!user_store_upsert(&users, 1'000'000'000'000ull + index, utf8_literal("Display Name"),
                               utf8_literal("username"))
                 .ok()) {
            return 14;
        }
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kUserUpsertIterations; ++iteration) {
        const UserStoreResult upserted = user_store_upsert(
            &users, 1'000'000'000'000ull + ((iteration * 2'981u) & (kUserCount - 1)), utf8_literal("Display Name"),
            utf8_literal("username"));
        if (!upserted.ok() || upserted.name_changed) {
            return 14;
        }
        checksum += users.clock;
    }
    const u64 user_unchanged_us = clock_microseconds(clock, begin, clock_now());
    begin = clock_now();
    for (u32 iteration = 0; iteration < kUserUpsertIterations / 4; ++iteration) {
        const UserStoreResult upserted = user_store_upsert(
            &users, 1'000'000'000'000ull + ((iteration * 2'981u) & (kUserCount - 1)),
            ((iteration >> 8) & 1u) ? utf8_literal("Other Name") : utf8_literal("Display Name Longer"),
            utf8_literal("username"));
        if (!upserted.ok() || !upserted.name_changed) {
            return 14;
        }
        checksum += users.compactions;
    }
    const u64 user_changed_us = clock_microseconds(clock, begin, clock_now());
    GuildMemberStore members{};
    if (!guild_member_store_initialize(&members, Slice<GuildMemberRecord>{g_members, 512},
                                       Slice<u32>{g_member_order, 512})) {
        return 15;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMemberIterations; ++iteration) {
        const u32 index = iteration % kMemberCount;
        const GuildMemberStoreResult upserted = guild_member_store_upsert(
            &members, state, 500, 9'000 + index, utf8_literal("nick"), {}, false, GuildMemberStatus::online, index,
            77);
        if (!upserted.ok()) {
            return 15;
        }
        checksum += members.clock;
    }
    const u64 member_upsert_us = clock_microseconds(clock, begin, clock_now());
    g_model_checksum = checksum;
    if (append_us == 0 || lookup_us == 0 || ordered_us == 0 || update_us == 0 || state_append_us == 0 ||
        state_lookup_us == 0 || state_update_us == 0 || permission_us == 0 ||
        activity_us == 0 || rich_update_us == 0 || rich_lookup_us == 0 || rich_reaction_us == 0 ||
        user_unchanged_us == 0 || user_changed_us == 0 || member_upsert_us == 0 ||
        g_model_checksum == 0) {
        return 13;
    }
    write_ops("message_sorted_append", per_second(kMessageItems, append_us));
    write_ops("message_find", per_second(kLookupIterations, lookup_us));
    write_ops("message_ordered_resolve", per_second(kOrderedIterations, ordered_us));
    write_ops("message_same_content_update", per_second(kUpdateIterations, update_us));
    write_ops("chat_state_bootstrap", per_second(kStateGuilds + kStateChannels, state_append_us));
    write_ops("chat_state_find", per_second(kStateLookupIterations, state_lookup_us));
    write_ops("chat_state_same_name_update", per_second(kStateUpdateIterations, state_update_us));
    write_ops("permission_recompute", per_second(kPermissionIterations, permission_us));
    write_ops("activity_mutation", per_second(activity_operations, activity_us));
    write_ops("message_rich_full_update", per_second(kRichUpdateIterations, rich_update_us));
    write_ops("message_rich_find", per_second(kRichLookupIterations, rich_lookup_us));
    write_ops("message_rich_reaction_mutation", per_second(kRichReactionIterations, rich_reaction_us));
    write_ops("user_upsert_unchanged", per_second(kUserUpsertIterations, user_unchanged_us));
    write_ops("user_upsert_changed", per_second(kUserUpsertIterations / 4, user_changed_us));
    write_ops("member_upsert_listed", per_second(kMemberIterations, member_upsert_us));
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_model_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
