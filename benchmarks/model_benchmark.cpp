#include "maicord/core/time.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/model/message_store.hpp"

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
volatile u64 g_model_checksum = 0;

u32 text_length(const char* value) noexcept {
    u32 length = 0;
    while (value[length]) {
        ++length;
    }
    return length;
}

void write_text(const char* value) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), value, text_length(value), &written, nullptr);
}

void write_u64(u64 value) noexcept {
    char digits[24]{};
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char output[24]{};
    for (u32 index = 0; index < count; ++index) {
        output[index] = digits[count - index - 1];
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, count, &written, nullptr);
}

void write_metric(const char* name, u64 value) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text(" ops/s\r\n");
}

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
            if (!chat_state_store_mark_channel_read(&permission_state,
                                                       activity_channel_id)) {
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
    begin = clock_now();
    for (u32 iteration = 0; iteration < kRichLookupIterations; ++iteration) {
        const MessageRichRecord* rich =
            message_rich_store_find(rich_store, first_id);
        if (!rich) {
            return 12;
        }
        checksum += rich->message_revision + rich->reaction_count;
    }
    const u64 rich_lookup_us =
        clock_microseconds(clock, begin, clock_now());

    g_model_checksum = checksum;
    if (append_us == 0 || lookup_us == 0 || ordered_us == 0 || update_us == 0 || state_append_us == 0 ||
        state_lookup_us == 0 || state_update_us == 0 || permission_us == 0 ||
        activity_us == 0 || rich_update_us == 0 || rich_lookup_us == 0 ||
        g_model_checksum == 0) {
        return 13;
    }
    write_metric("message_sorted_append", static_cast<u64>(kMessageItems) * 1'000'000ull / append_us);
    write_metric("message_find", static_cast<u64>(kLookupIterations) * 1'000'000ull / lookup_us);
    write_metric("message_ordered_resolve",
                 static_cast<u64>(kOrderedIterations) * 1'000'000ull / ordered_us);
    write_metric("message_same_content_update",
                 static_cast<u64>(kUpdateIterations) * 1'000'000ull / update_us);
    write_metric("chat_state_bootstrap",
                 static_cast<u64>(kStateGuilds + kStateChannels) * 1'000'000ull / state_append_us);
    write_metric("chat_state_find",
                 static_cast<u64>(kStateLookupIterations) * 1'000'000ull / state_lookup_us);
    write_metric("chat_state_same_name_update",
                 static_cast<u64>(kStateUpdateIterations) * 1'000'000ull / state_update_us);
    write_metric("permission_recompute",
                 static_cast<u64>(kPermissionIterations) * 1'000'000ull / permission_us);
    write_metric("activity_mutation",
                 activity_operations * 1'000'000ull / activity_us);
    write_metric("message_rich_full_update",
                 static_cast<u64>(kRichUpdateIterations) * 1'000'000ull /
                     rich_update_us);
    write_metric("message_rich_find",
                 static_cast<u64>(kRichLookupIterations) * 1'000'000ull /
                     rich_lookup_us);
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_model_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
