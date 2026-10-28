#include "maicord/model/message_rich_store.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/model/user_profile.hpp"

namespace maicord {
namespace {
struct ForwardFixture {
    u32 structurals[4096];
    char decoded[16384];
    GatewayMessageAttachment outer_attachments[10], snapshot_attachments[10];
    GatewayMessageEmbed outer_embeds[10], snapshot_embeds[10];
    GatewayMessageEmbedField outer_fields[64], snapshot_fields[64];
    GatewayMessageSticker outer_stickers[3], snapshot_stickers[3];
    GatewayMessageComponent outer_components[40], snapshot_components[40];
    GatewayMessageNamedUser outer_users[100], snapshot_users[100];
    MessageRichRecord records[8];
    u16 order[4];
    u8 owned_text[8192];
} fixture{};
GatewayMessageRichScratch rich_scratch() noexcept {
    return {{},
            {fixture.outer_attachments, 10},
            {fixture.outer_embeds, 10},
            {fixture.outer_fields, 64},
            {fixture.outer_stickers, 3},
            {fixture.outer_components, 40},
            {{fixture.snapshot_attachments, 10},
             {fixture.snapshot_embeds, 10},
             {fixture.snapshot_fields, 64},
             {fixture.snapshot_stickers, 3},
             {fixture.snapshot_components, 40},
             {fixture.snapshot_users, 100}}};
}
GatewayEventDecodeResult decode(Utf8View body, GatewayMessageEvent* event) noexcept {
    u64 roles[100]{}, member_roles[100]{};
    const auto rich = rich_scratch();
    auto result = gateway_decode_message_create(
        {body, 0, JsonValueKind::object}, {fixture.structurals, 4096}, 32, {fixture.decoded, 16384},
        {roles, 100}, {member_roles, 100}, {fixture.outer_users, 100}, rich, 7, event);
    event->mention_roles = {};
    event->member_roles = {};
    return result;
}
constexpr auto forward = utf8_literal(
    R"({"id":"900","channel_id":"7","author":{"id":"11","username":"sender"},"content":"outer note","timestamp":"2026-10-05T00:00:00Z","flags":16384,"message_reference":{"type":1,"channel_id":"9","message_id":"800"},"attachments":[{"id":"901","filename":"outer.png","size":64,"url":"https://cdn.discordapp.com/attachments/7/901/outer.png"}],"message_snapshots":[{"message":{"type":19,"content":"**snapshot** <@7>","timestamp":"2026-01-01T00:00:00Z","edited_timestamp":null,"flags":4,"mentions":[{"id":"7","username":"nested mention"}],"attachments":[{"id":"902","filename":"snapshot.mp4","size":128,"url":"https://cdn.discordapp.com/attachments/9/902/snapshot.mp4","width":32,"height":24}],"embeds":[{"type":"rich","title":"Nested embed","description":"details","image":{"url":"https://example.com/image.png"},"fields":[{"name":"key","value":"value"}]}],"sticker_items":[{"id":"903","name":"nested sticker","format_type":1}],"components":[{"type":10,"content":"nested component"}]}}]})");
bool append_attachments(ProtocolWriter* writer, u64 start) noexcept {
    for (u32 i = 0; i < 10; ++i) {
        if ((i && !protocol_writer_append_byte(writer, ',')) ||
            !protocol_writer_append_literal(writer, "{\"id\":\"") ||
            !protocol_writer_append_u64(writer, start + i) ||
            !protocol_writer_append_literal(
                writer,
                "\",\"filename\":\"file.png\",\"size\":64,\"url\":\"https://example.com/file.png\"}"))
            return false;
    }
    return true;
}
} // namespace

void test_forwarded_message_snapshots(void (*check)(bool, const char*) noexcept) noexcept {
    GatewayMessageEvent event{};
    const auto initial_decode = decode(forward, &event);
    check(initial_decode.ok() && event.id == 900 && event.author_id == 11 && event.channel_id == 7 &&
              utf8_equal(event.content, utf8_literal("outer note")) && event.snapshot.present &&
              utf8_equal(event.snapshot.content, utf8_literal("**snapshot** <@7>")) &&
              !event.mentions_current_user && event.snapshot.mention_users.count == 1 &&
              event.snapshot.mention_users[0].id == 7,
          "forward snapshot keeps outer identity and separate content/mentions without author fabrication "
          "or ping");
    if (!initial_decode.ok())
        return; // a decoder regression must report failure, not dereference absent bodies
    check(event.attachments.count == 1 && event.snapshot.attachments.count == 1 &&
              event.snapshot.embeds.count == 1 && event.snapshot.embed_fields.count == 1 &&
              event.snapshot.stickers.count == 1 && event.snapshot.components.count == 1,
          "forward snapshot retains practical attachment/embed/sticker/component fidelity");
    char mailbox[16384]{};
    u8 envelope_text[4096]{};
    ProtocolWriter envelope_writer{{envelope_text, 4096}, 0};
    GatewayEnvelope envelope{};
    GatewayClientEvent routed{};
    ConstSlice<u8> payload{};
    u64 ids[100]{};
    // Wraps the body in a MESSAGE_CREATE envelope, routes it into the pointer-free mailbox, poisons the
    // source and rebuilds the event from the mailbox alone.
    const auto transport = [&](Utf8View body, u64 sequence) noexcept {
        envelope_writer.used = 0;
        const bool routed_ok =
            protocol_writer_append_literal(&envelope_writer, "{\"op\":0,\"s\":") &&
            protocol_writer_append_u64(&envelope_writer, sequence) &&
            protocol_writer_append_literal(&envelope_writer, ",\"t\":\"MESSAGE_CREATE\",\"d\":") &&
            protocol_writer_append_bytes(&envelope_writer, {reinterpret_cast<const u8*>(body.data), body.bytes}) &&
            protocol_writer_append_byte(&envelope_writer, '}') &&
            gateway_decode_envelope({reinterpret_cast<const char*>(envelope_text), envelope_writer.used},
                                    {fixture.structurals, 4096}, 32, &envelope)
                .ok() &&
            gateway_client_route_dispatch(envelope, {fixture.structurals, 4096}, 32, {mailbox, 16384},
                                          {ids, 100}, 7, &routed, &payload)
                .ok();
        for (auto& byte : envelope_text)
            byte = 0xa5;
        return routed_ok && gateway_client_event_as_message(routed, payload, rich_scratch(), &event);
    };
    bool transported = transport(forward, 1);
    check(transported && event.snapshot.present && event.snapshot.attachments.count == 1 &&
              event.snapshot.embeds.count == 1 && event.snapshot.mention_users.count == 1 &&
              utf8_equal(event.snapshot.content, utf8_literal("**snapshot** <@7>")) &&
              !event.mentions_current_user,
          "Gateway pointer-free mailbox preserves snapshot text/media/mentions after JSON reuse");
    struct SnapshotUsers {
        u32 count;
        u64 id;
        Utf8View name;
    } snapshot_users{};
    const auto collect = [](void* context, u64 id, Utf8View name) noexcept {
        auto& users = *static_cast<SnapshotUsers*>(context);
        if (!users.count++) {
            users.id = id;
            users.name = name;
        }
        return true;
    };
    check(transported &&
              gateway_client_event_visit_message_snapshot_mention_users(routed, payload, &snapshot_users, collect) &&
              snapshot_users.count == 1 && snapshot_users.id == 7 &&
              utf8_equal(snapshot_users.name, utf8_literal("nested mention")),
          "snapshot named-user hydration reads validated scoped names without media decoding");
    if (transported) {
        const u32 snapshot_offset =
            routed.message.transient_bytes + routed.message.mention_role_count * sizeof(u64) +
            routed.message.member_role_count * sizeof(u64) +
            routed.message.mention_user_count * sizeof(GatewayClientNamedUserMutation) +
            sizeof(GatewayClientMessageRichHeader) + sizeof(GatewayClientMessageAttachmentMutation);
        constexpr u32 presence_offset = 40;
        const char original = mailbox[snapshot_offset + presence_offset];
        mailbox[snapshot_offset + presence_offset] = 2;
        GatewayMessageEvent invalid_transport{};
        check(!gateway_client_event_as_message(routed, payload, rich_scratch(), &invalid_transport),
              "Gateway snapshot mailbox rejects corrupt presence discriminator");
        mailbox[snapshot_offset + presence_offset] = original;
        const u32 user_reserved_offset = snapshot_offset + sizeof(GatewayClientMessageSnapshotHeader) + 14;
        mailbox[user_reserved_offset] = 1;
        SnapshotUsers rejected{};
        check(!gateway_client_event_visit_message_snapshot_mention_users(routed, payload, &rejected, collect) &&
                  !rejected.count,
              "snapshot hydration rejects invalid user list before exposing any identities");
        mailbox[user_reserved_offset] = 0;
    }
    MessageRichStore store{};
    check(message_rich_store_initialize(&store, {fixture.records, 8}, {fixture.order, 4},
                                        {fixture.owned_text, 8192}) &&
              store.capacity == 4 && store.record_capacity == 8 &&
              message_rich_store_apply(&store, 900, 1, event).ok(),
          "forward snapshot admits full child body in same physical rich store");
    const auto* outer = message_rich_store_find(store, 900);
    const auto* child = message_rich_store_snapshot(store, 900);
    check(outer && child && store.count == 1 && store.snapshot_count == 1 && outer->attachment_count == 1 &&
              child->attachment_count == 1 && child->embed_count == 1 && child->sticker_count == 1 &&
              child->component_count == 1 && child->snapshot.flags == 4 && child->snapshot.type == 19 &&
              !message_rich_has_header(*outer) && !message_rich_has_header(*child) &&
              !message_rich_header_user(*child),
          "snapshot child shares text/record owner while keeping full media and no reply author header");
    if (!outer || !child)
        return;
    bool changed = false;
    check(message_rich_store_set_attachment_dimensions(&store, 900, 1, 902, 64, 48, &changed, true) &&
              changed &&
              !message_rich_store_set_attachment_dimensions(&store, 900, 1, 902, 64, 48, &changed),
          "snapshot attachment dimensions use explicit child scope");
    GatewayMessageEvent update{};
    update.id = 900;
    update.fields = gateway_message_field_attachments;
    check(message_rich_store_apply(&store, 900, 2, update).ok() && message_rich_store_find(store, 900) &&
              !message_rich_store_find(store, 900)->attachment_count &&
              message_rich_store_snapshot(store, 900) &&
              message_rich_store_snapshot(store, 900)->attachment_count == 1 &&
              message_rich_store_snapshot(store, 900)->message_revision == 2,
          "partial outer media replacement retains immutable snapshot and advances child revision");
    check(message_rich_store_advance_revision(&store, 900, 2, 3) &&
              message_rich_store_snapshot(store, 900) &&
              message_rich_store_snapshot(store, 900)->message_revision == 3,
          "non-rich outer updates advance exact child revision ownership");
    for (u32 i = 0; i < 100; ++i) {
        event.snapshot.content =
            utf8_literal("snapshot text retained through repeated replacement and shared bank compaction");
        check(message_rich_store_apply(&store, 900, 4 + i, event).ok(),
              "snapshot repeated replacement remains admitted");
    }
    child = message_rich_store_snapshot(store, 900);
    check(
        store.compactions && child &&
            utf8_equal(message_rich_store_string(store, child->snapshot.content), event.snapshot.content) &&
            utf8_equal(message_rich_store_string(store, child->attachments[0].filename),
                       utf8_literal("snapshot.mp4")),
        "shared rich text compaction relocates snapshot body and media strings");
    update.fields = gateway_message_field_message_snapshots;
    check(message_rich_store_apply(&store, 900, 104, update).ok() &&
              !message_rich_store_snapshot(store, 900) && !store.snapshot_count,
          "explicit empty snapshots removes child while omission retained it");
    check(message_rich_store_apply(&store, 900, 105, event).ok() &&
              message_rich_store_delete(&store, 900) && !store.count && !store.snapshot_count &&
              !message_rich_store_snapshot(store, 900),
          "outer deletion releases snapshot child lifetime");

    u8 generated[4096]{};
    ProtocolWriter writer{{generated, 4096}, 0};
    bool written =
        protocol_writer_append_literal(
            &writer, "{\"id\":\"900\",\"channel_id\":\"7\",\"author\":{\"id\":\"11\"},\"content\":\"note\","
                     "\"timestamp\":\"2026-01-01T00:00:00Z\",\"attachments\":[") &&
        append_attachments(&writer, 1000) &&
        protocol_writer_append_literal(
            &writer, "],\"message_snapshots\":[{\"message\":{\"content\":\"body\",\"attachments\":[") &&
        append_attachments(&writer, 2000) && protocol_writer_append_literal(&writer, "]}}]}");
    check(written && decode({reinterpret_cast<const char*>(generated), writer.used}, &event).ok() &&
              event.attachments.count == 10 && event.snapshot.attachments.count == 10 &&
              !event.rich_truncations && !event.snapshot.rich_truncations,
          "outer and snapshot each preserve independent full attachment capacity");

    constexpr auto invalid = utf8_literal(
        R"({"id":"900","channel_id":"7","author":{"id":"11"},"content":"","timestamp":"2026-01-01T00:00:00Z","message_snapshots":[{"message":{"content":"nested","message_snapshots":[{"message":{"content":"too deep"}}]}}]})");
    check(!decode(invalid, &event).ok(), "nested snapshot beyond one level is rejected before publication");
    constexpr auto duplicate = utf8_literal(
        R"({"id":"900","channel_id":"7","author":{"id":"11"},"content":"","timestamp":"2026-01-01T00:00:00Z","message_snapshots":[{"message":{},"message":{}}]})");
    check(!decode(duplicate, &event).ok(), "duplicate snapshot message wrapper rejected");
    constexpr auto two = utf8_literal(
        R"({"id":"900","channel_id":"7","author":{"id":"11"},"content":"","timestamp":"2026-01-01T00:00:00Z","message_snapshots":[{"message":{}},{"message":{}}]})");
    check(!decode(two, &event).ok(), "unsupported multiple snapshots rejected");
    constexpr auto author = utf8_literal(
        R"({"id":"900","channel_id":"7","author":{"id":"11"},"content":"","timestamp":"2026-01-01T00:00:00Z","message_snapshots":[{"message":{"author":{"id":"12"}}}]})");
    check(!decode(author, &event).ok(), "snapshot cannot fabricate original author identity");

    MessageRichRecord one[1]{};
    u16 one_order[1]{};
    u8 one_text[128]{};
    MessageRichStore limited{};
    check(message_rich_store_initialize(&limited, {one, 1}, {one_order, 1}, {one_text, 128}) &&
              decode(forward, &event).ok() && !message_rich_store_apply(&limited, 900, 1, event).ok() &&
              !limited.count && !limited.snapshot_count && !limited.text_used,
          "child capacity denial cannot publish a partial parent or lose snapshot media");
    StableStorage owned{};
    MessageRichStore lazy{};
    u16 lazy_order[4]{};
    u8 lazy_text[8192]{};
    check(message_rich_store_initialize_owned_sparse(&lazy, &owned, 4, {lazy_order, 4}, {lazy_text, 8192}) &&
              lazy.capacity == 4 && lazy.record_capacity == 8 && !owned.arena.committed &&
              !lazy.snapshot_count,
          "snapshot physical headroom reserves without committing child records");
    GatewayMessageEvent plain{};
    plain.id = 901;
    plain.fields = gateway_message_field_content;
    plain.content = utf8_literal("plain");
    check(message_rich_store_apply(&lazy, 901, 1, plain).ok() && !owned.arena.committed &&
              !lazy.record_extent,
          "ordinary text-only message retains zero lazy rich commitment after snapshot extension");
    check(message_rich_store_apply(&lazy, 900, 1, event).ok() && lazy.count == 1 &&
              lazy.snapshot_count == 1 && message_rich_store_reset(&lazy) && !lazy.count &&
              !lazy.snapshot_count,
          "lazy typed child admission and account reset clear both lifetimes");
    message_rich_store_release(&lazy);
    constexpr auto audio = utf8_literal(
        R"({"id":"950","channel_id":"7","author":{"id":"11"},"content":"","timestamp":"2026-10-05T00:00:00Z","flags":8192,"attachments":[{"id":"951","filename":"voice.ogg","content_type":"audio/ogg","size":128,"url":"https://example.com/voice.ogg","duration_secs":1.2349,"waveform":"AP+A"}],"message_snapshots":[{"message":{"flags":8192,"attachments":[{"id":"952","filename":"child.opus","size":64,"url":"https://example.com/child.opus","duration_secs":1.2e-3,"waveform":"AA=="}]}}]})");
    check(decode(audio, &event).ok() && event.flags == kGatewayMessageVoiceFlag &&
              event.attachments.count == 1 && event.attachments[0].has_duration &&
              event.attachments[0].duration_ms == 1234 &&
              utf8_equal(event.attachments[0].waveform, utf8_literal("AP+A")) &&
              event.snapshot.attachments[0].has_duration && event.snapshot.attachments[0].duration_ms == 1,
          "audio attachment decimal duration and waveform retain independent forwarded voice metadata");
    transported = transport(audio, 2);
    check(transported && event.attachments[0].duration_ms == 1234 && event.attachments[0].has_duration &&
              utf8_equal(event.attachments[0].waveform, utf8_literal("AP+A")) &&
              utf8_equal(event.snapshot.attachments[0].waveform, utf8_literal("AA==")),
          "audio pointer-free Gateway mutation preserves duration and both waveform ranges after source "
          "reuse");

    check(transported && message_rich_store_apply(&store, 950, 1, event).ok() &&
              message_rich_store_find(store, 950) && message_rich_store_snapshot(store, 950) &&
              message_rich_record_voice(*message_rich_store_find(store, 950)) &&
              message_rich_record_voice(*message_rich_store_snapshot(store, 950)),
          "voice authority is retained independently in outer and forwarded rich records without size "
          "growth");
    for (auto& byte : mailbox)
        byte = '!';
    check(message_rich_store_find(store, 950) && message_rich_store_snapshot(store, 950) &&
              utf8_equal(message_rich_store_string(
                             store, message_rich_store_find(store, 950)->attachments[0].waveform),
                         utf8_literal("AP+A")) &&
              utf8_equal(message_rich_store_string(
                             store, message_rich_store_snapshot(store, 950)->attachments[0].waveform),
                         utf8_literal("AA==")),
          "outer and snapshot waveform ownership survives complete pointer-free mailbox reuse");
    GatewayMessageEvent voice_update{};
    voice_update.id = 950;
    voice_update.fields = gateway_message_field_flags;
    check(message_rich_store_apply(&store, 950, 2, voice_update).ok() &&
              !message_rich_record_voice(*message_rich_store_find(store, 950)) &&
              message_rich_record_voice(*message_rich_store_snapshot(store, 950)),
          "flags-only update clears enclosing voice while omitted forwarded snapshot retains its "
          "independent authority");

    const auto decode_audio_fields = [&](Utf8View fields) noexcept {
        writer.used = 0;
        return protocol_writer_append_literal(
                   &writer,
                   "{\"id\":\"950\",\"channel_id\":\"7\",\"author\":{\"id\":\"11\"},\"content\":\"\","
                   "\"timestamp\":\"2026-10-05T00:00:00Z\",\"attachments\":[{\"id\":\"951\",\"filename\":"
                   "\"audio.ogg\",\"size\":128,\"url\":\"https://example.com/audio.ogg\",") &&
               protocol_writer_append_bytes(&writer,
                                            {reinterpret_cast<const u8*>(fields.data), fields.bytes}) &&
               protocol_writer_append_literal(&writer, "}]}") &&
               decode({reinterpret_cast<const char*>(generated), writer.used}, &event).ok();
    };
    check(decode_audio_fields(utf8_literal("\"duration_secs\":0,\"waveform\":null")) &&
              event.attachments[0].has_duration && !event.attachments[0].duration_ms &&
              !event.attachments[0].waveform.bytes,
          "audio zero duration is known while null waveform is absent");
    check(decode_audio_fields(utf8_literal("\"duration_secs\":null,\"waveform\":\"\"")) &&
              !event.attachments[0].has_duration && !event.attachments[0].duration_ms,
          "audio null duration remains unknown and empty waveform supplies no invented samples");
    check(decode_audio_fields(utf8_literal("\"duration_secs\":4294967.295")) &&
              event.attachments[0].duration_ms == ~0u,
          "audio duration maximum representable millisecond boundary remains exact");
    check(decode_audio_fields(utf8_literal("\"duration_secs\":1e-100")) &&
              event.attachments[0].has_duration && !event.attachments[0].duration_ms,
          "audio tiny finite duration floors below a millisecond without floating CRT conversion");
    check(!decode_audio_fields(utf8_literal("\"duration_secs\":-1")) &&
              !decode_audio_fields(utf8_literal("\"duration_secs\":4294967.296")) &&
              !decode_audio_fields(utf8_literal("\"duration_secs\":1e100")) &&
              !decode_audio_fields(utf8_literal("\"duration_secs\":\"1\"")) &&
              !decode_audio_fields(utf8_literal("\"duration_secs\":1,\"duration_secs\":2")),
          "audio negative overflowing wrong-type and duplicate durations reject before publication");
    check(!decode_audio_fields(utf8_literal("\"waveform\":\"TR==\"")) &&
              !decode_audio_fields(utf8_literal("\"waveform\":\"AA=\"")) &&
              !decode_audio_fields(utf8_literal("\"waveform\":\"AA-_\"")) &&
              !decode_audio_fields(utf8_literal("\"waveform\":42")) &&
              !decode_audio_fields(utf8_literal("\"waveform\":\"AA==\",\"waveform\":\"AA==\"")),
          "audio waveform rejects noncanonical padding alphabet wrong type and duplicate metadata");
    u8 samples[257]{};
    char encoded[348]{};
    u8 fields[512]{};
    auto base64 = base64_encode({samples, 256}, {encoded, sizeof(encoded)});
    ProtocolWriter field_writer{{fields, sizeof(fields)}, 0};
    bool fields_written =
        base64.ok() && protocol_writer_append_literal(&field_writer, "\"waveform\":\"") &&
        protocol_writer_append_bytes(&field_writer, {reinterpret_cast<const u8*>(encoded), base64.bytes}) &&
        protocol_writer_append_byte(&field_writer, '"');
    check(fields_written &&
              decode_audio_fields({reinterpret_cast<const char*>(fields), field_writer.used}) &&
              event.attachments[0].waveform.bytes == 344 &&
              gateway_message_waveform_valid(event.attachments[0].waveform),
          "audio waveform accepts exact 256-sample canonical boundary without inline attachment array");
    base64 = base64_encode({samples, 257}, {encoded, sizeof(encoded)});
    field_writer.used = 0;
    fields_written =
        base64.ok() && protocol_writer_append_literal(&field_writer, "\"waveform\":\"") &&
        protocol_writer_append_bytes(&field_writer, {reinterpret_cast<const u8*>(encoded), base64.bytes}) &&
        protocol_writer_append_byte(&field_writer, '"');
    check(fields_written &&
              !decode_audio_fields({reinterpret_cast<const char*>(fields), field_writer.used}),
          "audio waveform rejects 257 samples even when Base64 still fits the 344-byte encoded limit");
}
} // namespace maicord
