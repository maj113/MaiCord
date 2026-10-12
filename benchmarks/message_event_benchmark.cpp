#include "maicord/core/time.hpp"
#include "maicord/protocol/gateway_message_events.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kIterations = 500'000;
inline constexpr u32 kRichIterations = 100'000;
inline constexpr Utf8View kCreatePayload = utf8_literal(
    R"({"op":0,"d":{"id":"1270000000000000001","channel_id":"1260000000000000001","author":{"id":"1250000000000000001","username":"benchmark"},"content":"bounded native message event payload","timestamp":"2024-08-30T12:34:56.789123+02:30","edited_timestamp":null,"flags":4,"mention_everyone":false,"mentions":[{"id":"1250000000000000042","username":"self"}],"mention_roles":["1250000000000000055","1250000000000000066"],"attachments":[],"embeds":[]},"s":9,"t":"MESSAGE_CREATE"})");
inline constexpr Utf8View kUpdatePayload = utf8_literal(
    R"({"op":0,"d":{"id":"1270000000000000001","channel_id":"1260000000000000001","content":"bounded native message event payload","edited_timestamp":"2024-08-30T10:05:06.007Z","flags":8},"s":10,"t":"MESSAGE_UPDATE"})");
inline constexpr Utf8View kRichPayload = utf8_literal(
    R"({"op":0,"d":{"id":"1270000000000000001","channel_id":"1260000000000000001","author":{"id":"1250000000000000001","username":"benchmark"},"content":"bounded rich message","timestamp":"2024-08-30T12:34:56Z","message_reference":{"message_id":"1270000000000000000","channel_id":"1260000000000000001"},"referenced_message":{"id":"1270000000000000000","channel_id":"1260000000000000001","author":{"id":"1250000000000000002","username":"source"},"content":"referenced text"},"reactions":[{"count":4,"count_details":{"normal":3,"burst":1},"me":true,"emoji":{"id":"44","name":"party","animated":true}}],"attachments":[{"id":"70","filename":"clip.mp4","content_type":"video/mp4","size":12345,"url":"https://cdn.discordapp.com/attachments/7/70/clip.mp4","width":1280,"height":720}],"embeds":[{"title":"Build","type":"rich","description":"green","author":{"name":"CI"},"footer":{"text":"done"},"fields":[{"name":"Target","value":"Release","inline":true}]}],"sticker_items":[{"id":"88","name":"Wave","format_type":2}],"components":[{"type":17,"id":1,"components":[{"type":10,"id":2,"content":"Component markdown"},{"type":2,"id":3,"style":1,"label":"Native button","custom_id":"benchmark"}]}]},"s":11,"t":"MESSAGE_CREATE"})");

volatile u64 g_message_event_checksum = 0;

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

void write_metric(const char* name, u64 value, const char* unit) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text(" ");
    write_text(unit);
    write_text("\r\n");
}

bool decode_message(Utf8View payload, bool create, GatewayMessageEvent* message,
                     u32* structural_scratch, char* content_scratch,
                     u64* mention_role_scratch) noexcept {
    GatewayEnvelope envelope{};
    const GatewayDecodeResult envelope_result = gateway_decode_envelope(
        payload, Slice<u32>{structural_scratch, 256}, 16, &envelope);
    if (!envelope_result.ok()) {
        return false;
    }
    const GatewayEventDecodeResult event_result =
        create ? gateway_decode_message_create(
                     envelope.data, Slice<u32>{structural_scratch, 256}, 16,
                     Slice<char>{content_scratch, 256},
                     Slice<u64>{mention_role_scratch, kGatewayMessageMentionRoleMaximumIds},
                     1'250'000'000'000'000'042ull, message)
               : gateway_decode_message_update(
                     envelope.data, Slice<u32>{structural_scratch, 256}, 16,
                     Slice<char>{content_scratch, 256},
                     Slice<u64>{mention_role_scratch, kGatewayMessageMentionRoleMaximumIds},
                     1'250'000'000'000'000'042ull, message);
    return event_result.ok();
}

bool decode_rich_message(
    GatewayMessageEvent* message, u32* structural_scratch,
    char* content_scratch, u64* mention_role_scratch,
    u64* member_role_scratch,
    GatewayMessageNamedUser* mention_users,
    GatewayMessageReaction* reactions,
    GatewayMessageAttachment* attachments, GatewayMessageEmbed* embeds,
    GatewayMessageEmbedField* embed_fields, GatewayMessageSticker* stickers,
    GatewayMessageComponent* components) noexcept {
    GatewayEnvelope envelope{};
    if (!gateway_decode_envelope(
             kRichPayload, Slice<u32>{structural_scratch, 2'048}, 32,
             &envelope)
             .ok()) {
        return false;
    }
    const GatewayMessageRichScratch rich_scratch{
        Slice<GatewayMessageReaction>{reactions,
                                      kGatewayMessageReactionMaximum},
        Slice<GatewayMessageAttachment>{attachments,
                                        kGatewayMessageAttachmentMaximum},
        Slice<GatewayMessageEmbed>{embeds, kGatewayMessageEmbedMaximum},
        Slice<GatewayMessageEmbedField>{
            embed_fields, kGatewayMessageEmbedFieldMaximum},
        Slice<GatewayMessageSticker>{stickers,
                                     kGatewayMessageStickerMaximum},
        Slice<GatewayMessageComponent>{components,
                                       kGatewayMessageComponentMaximum}};
    return gateway_decode_message_create(
               envelope.data, Slice<u32>{structural_scratch, 2'048}, 32,
               Slice<char>{content_scratch, 16 * 1024},
               Slice<u64>{mention_role_scratch,
                          kGatewayMessageMentionRoleMaximumIds},
               Slice<u64>{member_role_scratch,
                          kGatewayMessageMemberRoleMaximumIds},
               Slice<GatewayMessageNamedUser>{
                   mention_users, kGatewayMessageMentionUserMaximum},
               rich_scratch, 0, message)
        .ok();
}

int run() noexcept {
    u32 structural_scratch[2'048]{};
    char content_scratch[16 * 1024]{};
    u64 mention_role_scratch[kGatewayMessageMentionRoleMaximumIds]{};
    u64 member_role_scratch[kGatewayMessageMemberRoleMaximumIds]{};
    GatewayMessageNamedUser mention_users[kGatewayMessageMentionUserMaximum]{};
    GatewayMessageReaction reactions[kGatewayMessageReactionMaximum]{};
    GatewayMessageAttachment attachments[kGatewayMessageAttachmentMaximum]{};
    GatewayMessageEmbed embeds[kGatewayMessageEmbedMaximum]{};
    GatewayMessageEmbedField embed_fields[kGatewayMessageEmbedFieldMaximum]{};
    GatewayMessageSticker stickers[kGatewayMessageStickerMaximum]{};
    GatewayMessageComponent components[kGatewayMessageComponentMaximum]{};
    GatewayMessageEvent message{};
    const Clock clock = clock_initialize();
    u64 checksum = 0;

    i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kIterations; ++iteration) {
        if (!decode_message(kCreatePayload, true, &message, structural_scratch,
                            content_scratch, mention_role_scratch)) {
            return 1;
        }
        checksum += message.id + message.author_id + message.content.bytes +
                    message.timestamp_ms + message.fields + message.mention_roles.count +
                    static_cast<u64>(message.mentions_current_user);
    }
    const u64 create_us = clock_microseconds(clock, begin, clock_now());

    begin = clock_now();
    for (u32 iteration = 0; iteration < kIterations; ++iteration) {
        if (!decode_message(kUpdatePayload, false, &message, structural_scratch,
                            content_scratch, mention_role_scratch)) {
            return 2;
        }
        checksum += message.id + message.edited_timestamp_ms + message.flags +
                    message.content.bytes + message.fields;
    }
    const u64 update_us = clock_microseconds(clock, begin, clock_now());
    begin = clock_now();
    for (u32 iteration = 0; iteration < kRichIterations; ++iteration) {
        if (!decode_rich_message(
                &message, structural_scratch, content_scratch,
                mention_role_scratch, member_role_scratch, mention_users,
                reactions, attachments,
                embeds, embed_fields, stickers, components)) {
            return 3;
        }
        checksum += message.id + message.reference.message_id +
                    message.reactions.count + message.attachments.count +
                    message.embeds.count + message.embed_fields.count +
                    message.stickers.count + message.components.count;
    }
    const u64 rich_us = clock_microseconds(clock, begin, clock_now());
    g_message_event_checksum = checksum;
    if (create_us == 0 || update_us == 0 || rich_us == 0 ||
        g_message_event_checksum == 0) {
        return 4;
    }
    write_metric("message_create_decode",
                 static_cast<u64>(kIterations) * 1'000'000ull / create_us, "events/s");
    write_metric("message_create_decode_mib",
                 static_cast<u64>(kIterations) * kCreatePayload.bytes * 1'000'000ull /
                     create_us / (1024ull * 1024ull),
                 "MiB/s");
    write_metric("message_update_decode",
                 static_cast<u64>(kIterations) * 1'000'000ull / update_us, "events/s");
    write_metric("message_update_decode_mib",
                 static_cast<u64>(kIterations) * kUpdatePayload.bytes * 1'000'000ull /
                     update_us / (1024ull * 1024ull),
                 "MiB/s");
    write_metric("message_rich_decode",
                 static_cast<u64>(kRichIterations) * 1'000'000ull /
                     rich_us,
                 "events/s");
    write_metric("message_rich_decode_mib",
                 static_cast<u64>(kRichIterations) * kRichPayload.bytes *
                     1'000'000ull / rich_us / (1024ull * 1024ull),
                 "MiB/s");
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_message_event_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
