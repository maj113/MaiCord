#include "maicord/app/message_history_sync.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/ui/message_markdown.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/message_rich_store.hpp"

namespace maicord {
namespace {
struct LiveFixture {
    MessageRecord records[12];
    u32 order[12], heights[12];
    u64 sums[12];
    u8 text[2048], rich_text[2048];
    MessageRichRecord rich_records[12];
    u16 rich_order[12];
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
};
LiveFixture fixture;
bool initialize() noexcept {
    auto& f = fixture;
    return message_store_initialize(&f.store, {f.records, 12}, {f.order, 12}, {f.text, sizeof(f.text)}) &&
           message_rich_store_initialize(&f.rich, {f.rich_records, 12}, {f.rich_order, 12},
                                         {f.rich_text, sizeof(f.rich_text)}) &&
           ui_variable_list_initialize(&f.list, {f.heights, 12}, {f.sums, 12}, 0, ui_extent_units(60)) &&
           message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(60),
                                       ui_extent_units(120), true) &&
           message_timeline_bind_rich_store(&f.timeline, &f.rich);
}
GatewayMessageEvent message(u64 id) noexcept {
    GatewayMessageEvent event{};
    event.id = id;
    event.channel_id = 7;
    event.author_id = 9;
    event.timestamp_ms = 1'700'000'000'000ull + id;
    event.content = utf8_literal("hello team");
    event.fields = gateway_message_field_id | gateway_message_field_channel_id |
                   gateway_message_field_author_id | gateway_message_field_timestamp |
                   gateway_message_field_content;
    return event;
}
const MessageRecord* record(u64 id) noexcept {
    const auto found = message_store_find(fixture.store, id);
    return found.ok() ? message_store_resolve(fixture.store, found.handle) : nullptr;
}
} // namespace

void test_message_live_updates(void (*check)(bool, const char*) noexcept) noexcept {
    char output[512]{};
    u32 styles[512]{};
    TextStyleSpan spans[64]{};
    MessageMarkdownSemantic semantics[8]{};
    const auto parse = [&](Utf8View source, const MessageMarkdownOptions& options = {}) noexcept {
        return message_markdown_parse(source, {output, 512}, {styles, 512}, {spans, 64}, options,
                                      {semantics, 8});
    };
    auto parsed = parse(utf8_literal("Thanks, see you there :heart: :+1: :laughing: :100:"));
    check(parsed.ok() &&
              utf8_equal(parsed.text,
                         utf8_literal("Thanks, see you there \342\235\244\357\270\217 "
                                      "\360\237\221\215 \360\237\230\206 \360\237\222\257")),
          "live_emoji_aliases_heart_plus_one_laughing_100");
    parsed = parse(utf8_literal("**:heart:** `:heart:` \\:heart: :unknownemoji:"));
    check(parsed.ok() &&
              utf8_equal(parsed.text,
                         utf8_literal("\342\235\244\357\270\217 :heart: :heart: :unknownemoji:")) &&
              (styles[0] & text_style_bold),
          "live_emoji_aliases_respect_markdown_code_escape_and_unknown");
    parsed = parse(utf8_literal(":v:"));
    check(parsed.ok() && parsed.text.bytes == 6 && (styles[0] & text_style_jumbo_emoji),
          "live_emoji_alias_expansion_and_jumbo");
    parsed = parse(utf8_literal(":thumbsup::skin-tone-2:"));
    check(parsed.ok() && utf8_equal(parsed.text, utf8_literal("\360\237\221\215\360\237\217\274")),
          "live_emoji_diversity_alias");
    parsed = parse(utf8_literal(":wave_tone3: :grinning_face_with_smiling_eyes: :smile::skin-tone-2:"));
    check(parsed.ok() && utf8_equal(parsed.text, utf8_literal("\360\237\221\213\360\237\217\275 "
                                                              "\360\237\230\204 \360\237\230\204:skin-tone-2:")),
          "live_emoji_tone_suffix_catalog_name_and_untoned_base");
    parsed = parse(utf8_literal(":flag_si:"));
    check(parsed.ok() && parsed.semantic_count == 1 &&
              semantics[0].kind == MessageMarkdownSemanticKind::regional_flag,
          "live_emoji_flag_alias_uses_flag_media_semantic");
    parsed = parse(utf8_literal("https://example.test/:heart: [:heart:](https://example.test/:heart:)"));
    check(parsed.ok() &&
              utf8_equal(parsed.text,
                         utf8_literal("https://example.test/:heart: \342\235\244\357\270\217")),
          "live_emoji_aliases_preserve_url_targets_and_transform_link_labels");
    MessageMarkdownOptions source_options{};
    source_options.preserve_entities = true;
    parsed = parse(utf8_literal(":heart:"), source_options);
    check(parsed.ok() && utf8_equal(parsed.text, utf8_literal(":heart:")),
          "live_emoji_editor_preserves_shortcode_source");

    auto& f = fixture;
    bool ready = initialize();
    for (u32 i = 0; ready && i < 6; ++i)
        ready = message_timeline_apply_created(&f.timeline, message(100 + i)).ok();
    bool changed = true;
    check(ready && message_timeline_scroll_by(&f.timeline, 0, ui_extent_units(1), &changed) && !changed &&
              f.timeline.follow_bottom,
          "live_scroll_zero_distance_preserves_bottom_follow");
    auto reply = message(106);
    reply.content = utf8_literal("Thanks, see you there :heart:");
    reply.fields |= gateway_message_field_message_reference;
    reply.reference.message_id = 100;
    reply.reference.channel_id = 7;
    reply.reference.state = GatewayMessageReferenceState::unknown;
    const auto applied = message_timeline_apply_created(&f.timeline, reply);
    const auto* rich = message_rich_store_find(f.rich, 106);
    check(applied.ok() && rich && rich->reference.state == GatewayMessageReferenceState::available &&
              rich->reference.author_id == 9 &&
              utf8_equal(message_rich_store_string(f.rich, rich->reference.content),
                         utf8_literal("hello team")),
          "live_reply_omitted_snapshot_resolves_resident_target_immediately");
    check(f.timeline.follow_bottom && message_timeline_is_at_bottom(f.timeline, 0),
          "live_reply_insert_follows_new_bottom");
    MessageTimelineInvalidation measured{};
    check(message_timeline_set_item_height(&f.timeline, 6, ui_extent_units(150), &measured) &&
              message_timeline_is_at_bottom(f.timeline, 0),
          "live_reply_measured_height_stays_at_bottom");

    auto partial = reply;
    partial.fields = gateway_message_field_message_reference;
    partial.reference = {};
    partial.reference.message_id = 100;
    partial.reference.channel_id = 7;
    partial.reference.state = GatewayMessageReferenceState::unknown;
    check(message_rich_store_apply(&f.rich, 106, record(106)->revision, partial).ok() &&
              message_rich_store_find(f.rich, 106)->reference.state ==
                  GatewayMessageReferenceState::available,
          "live_reply_partial_reference_update_retains_cached_snapshot");
    GatewayClientEvent update{};
    update.dispatch_kind = GatewayClientDispatchKind::message_update;
    update.kind = GatewayClientEventKind::dispatch;
    update.dispatch_payload_bytes = 6;
    update.message.id = 106;
    update.message.channel_id = 7;
    update.message.content_bytes = 6;
    update.message.fields =
        gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_content;
    const auto edited = utf8_literal("edited");
    u64 bulk[1]{};
    check(message_timeline_apply(&f.timeline, update,
                                 {reinterpret_cast<const u8*>(edited.data), edited.bytes}, {bulk, 1})
                  .ok() &&
              message_rich_store_find(f.rich, 106)->message_revision == record(106)->revision,
          "live_reply_content_only_update_keeps_quote_revision_current");
    partial.fields |= gateway_message_field_referenced_message;
    partial.reference.state = GatewayMessageReferenceState::deleted;
    check(message_rich_store_apply(&f.rich, 106, record(106)->revision, partial).ok() &&
              message_rich_store_find(f.rich, 106)->reference.state ==
                  GatewayMessageReferenceState::deleted,
          "live_reply_explicit_deleted_snapshot_is_authoritative");

    check(message_timeline_set_scroll(&f.timeline, f.list.total_units, false) &&
              message_timeline_apply_created(&f.timeline, message(107)).ok() && f.timeline.follow_bottom &&
              message_timeline_is_at_bottom(f.timeline, 0),
          "live_scroll_restored_live_edge_rearms_follow_before_arrival");
    check(message_timeline_scroll_by(&f.timeline, -static_cast<i64>(ui_extent_units(90)), 0, &changed) &&
              !f.timeline.follow_bottom,
          "live_scroll_reading_older_disables_follow");
    const auto old_scroll = f.timeline.scroll_units;
    check(message_timeline_scroll_by(&f.timeline, 0, 0, &changed) &&
              message_timeline_apply_created(&f.timeline, message(108)).ok() && !f.timeline.follow_bottom &&
              f.timeline.scroll_units == old_scroll,
          "live_scroll_arrivals_preserve_older_reader_position");

    reply = message(109);
    reply.fields |= gateway_message_field_message_reference;
    reply.reference.message_id = 90;
    reply.reference.channel_id = 7;
    reply.reference.state = GatewayMessageReferenceState::unknown;
    check(message_timeline_apply_created(&f.timeline, reply).ok(),
          "live_reply_nonresident_reference_fixture");
    ReplyReferenceRefresh refresh{};
    check(message_history_reply_queue(&refresh, f.timeline, 7, 100) && refresh.message_id == 109 &&
              refresh.state == AttachmentRefreshState::request_required,
          "live_reply_nonresident_snapshot_queues_passive_refresh");
    constexpr auto body = utf8_literal(
        R"([{"id":"109","channel_id":"7","author":{"id":"9","username":"Claudia"},"content":"reply","timestamp":"2024-08-30T14:00:00Z","message_reference":{"message_id":"90","channel_id":"7"},"referenced_message":{"id":"90","channel_id":"7","author":{"id":"8","username":"Wren042"},"content":"hello team"}}])");
    u32 page_structurals[256]{}, message_structurals[256]{};
    char page_text[2048]{};
    GatewayMessageEvent messages[1]{};
    GatewayMessageNamedUser named[4]{};
    GatewayMessageReaction reactions[1]{};
    const MessageHistoryDecodeScratch scratch{{page_structurals, 256},
                                              {message_structurals, 256},
                                              {page_text, 2048},
                                              {messages, 1},
                                              {named, 4},
                                              {{reactions, 1}},
                                              32};
    refresh.state = AttachmentRefreshState::awaiting_response;
    RestClientResponse response{};
    response.status_code = 429;
    response.rate_limited = true;
    response.retry_at_ms = 200;
    auto accepted =
        message_history_reply_accept_rest(&refresh, &f.timeline, nullptr, response, {}, scratch);
    check(accepted.accepted() && accepted.disposition == MessageHistoryRestDisposition::retry_scheduled &&
              message_history_reply_queue(&refresh, f.timeline, 7, 199) &&
              refresh.state == AttachmentRefreshState::waiting_retry &&
              message_history_reply_queue(&refresh, f.timeline, 7, 200) &&
              refresh.state == AttachmentRefreshState::request_required,
          "live_reply_refresh_respects_rate_limit_deadline");
    refresh.state = AttachmentRefreshState::awaiting_response;
    response = {};
    response.status_code = 200;
    response.body_bytes = body.bytes;
    const auto before_refresh = f.timeline.scroll_units;
    accepted = message_history_reply_accept_rest(&refresh, &f.timeline, nullptr, response, body, scratch);
    rich = message_rich_store_find(f.rich, 109);
    check(accepted.accepted() && rich && rich->reference.state == GatewayMessageReferenceState::available &&
              utf8_equal(message_rich_store_string(f.rich, rich->reference.author_name),
                         utf8_literal("Wren042")) &&
              f.timeline.scroll_units == before_refresh &&
              utf8_equal(message_store_content(f.store, message_store_find(f.store, 109).handle),
                         utf8_literal("hello team")),
          "live_reply_refresh_updates_only_quote_without_scrolling_or_replacing_message");
    check(message_history_reply_queue(&refresh, f.timeline, 7, 300) &&
              refresh.state == AttachmentRefreshState::idle,
          "live_reply_refresh_does_not_repeat_completed_request");
    // A deletion/update that wins the race remains authoritative.
    partial.id = 109;
    partial.reference.message_id = 90;
    check(message_rich_store_apply(&f.rich, 109, record(109)->revision, partial).ok(),
          "live_reply_refresh_deletion_race_fixture");
    refresh.message_id = 109;
    refresh.reference_id = 90;
    refresh.state = AttachmentRefreshState::awaiting_response;
    accepted = message_history_reply_accept_rest(&refresh, &f.timeline, nullptr, response, body, scratch);
    check(accepted.accepted() && message_rich_store_find(f.rich, 109)->reference.state ==
                                     GatewayMessageReferenceState::deleted,
          "live_reply_refresh_cannot_resurrect_deleted_reference");
    check(initialize(), "live_voice_flags_timeline_fixture");
    GatewayMessageAttachment audio{};
    audio.id = 5000;
    audio.filename = utf8_literal("voice.ogg");
    audio.content_type = utf8_literal("audio/ogg");
    audio.url = utf8_literal("https://example.com/voice.ogg");
    audio.size = 128;
    audio.waveform = utf8_literal("AP+A");
    audio.duration_ms = 3000;
    audio.has_duration = true;
    auto voice = message(110);
    voice.content = {};
    voice.flags = kGatewayMessageVoiceFlag;
    voice.fields |= gateway_message_field_flags | gateway_message_field_attachments |
                    gateway_message_field_message_snapshots;
    voice.attachments = {&audio, 1};
    GatewayMessageAttachment child_audio = audio;
    child_audio.id = 5001;
    voice.snapshot.present = true;
    voice.snapshot.flags = kGatewayMessageVoiceFlag;
    voice.snapshot.fields = gateway_message_field_flags | gateway_message_field_attachments;
    voice.snapshot.attachments = {&child_audio, 1};
    check(message_timeline_apply_created(&f.timeline, voice).ok() && record(110) &&
              message_rich_store_find(f.rich, 110) && message_rich_store_snapshot(f.rich, 110) &&
              message_rich_record_voice(*message_rich_store_find(f.rich, 110)) &&
              message_rich_record_voice(*message_rich_store_snapshot(f.rich, 110)),
          "live_voice_timeline_creation_retains_enclosing_and_forwarded_voice_authority");
    const u32 text_used = f.rich.text_used;
    GatewayMessageEvent flag_update{};
    flag_update.id = 110;
    flag_update.channel_id = 7;
    flag_update.fields =
        gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_flags;
    check(
        message_timeline_apply_updated(&f.timeline, flag_update).ok() && record(110) &&
            !(record(110)->flags & kGatewayMessageVoiceFlag) && message_rich_store_find(f.rich, 110) &&
            !message_rich_record_voice(*message_rich_store_find(f.rich, 110)) &&
            message_rich_store_find(f.rich, 110)->message_revision == record(110)->revision &&
            message_rich_store_snapshot(f.rich, 110) &&
            message_rich_record_voice(*message_rich_store_snapshot(f.rich, 110)) &&
            f.rich.text_used == text_used &&
            message_rich_store_find(f.rich, 110)->attachments[0].duration_ms == 3000 &&
            utf8_equal(message_rich_store_string(
                           f.rich, message_rich_store_find(f.rich, 110)->attachments[0].waveform),
                       utf8_literal("AP+A")),
        "live_voice_flags_only_timeline_update_clears_outer_authority_preserves_child_and_audio_metadata");
    flag_update.flags = kGatewayMessageVoiceFlag;
    check(
        message_timeline_apply_updated(&f.timeline, flag_update).ok() && record(110) &&
            (record(110)->flags & kGatewayMessageVoiceFlag) && message_rich_store_find(f.rich, 110) &&
            message_rich_record_voice(*message_rich_store_find(f.rich, 110)) &&
            message_rich_store_find(f.rich, 110)->message_revision == record(110)->revision &&
            f.rich.text_used == text_used,
        "live_voice_flags_only_timeline_update_restores_authority_without_attachment_copy_or_text_growth");
    GatewayMessageEvent omitted_flags{};
    omitted_flags.id = 110;
    omitted_flags.channel_id = 7;
    omitted_flags.fields =
        gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_attachments;
    omitted_flags.attachments = {&audio, 1};
    check(message_timeline_apply_updated(&f.timeline, omitted_flags).ok() &&
              message_rich_store_find(f.rich, 110) &&
              message_rich_record_voice(*message_rich_store_find(f.rich, 110)) &&
              message_rich_store_snapshot(f.rich, 110) &&
              message_rich_record_voice(*message_rich_store_snapshot(f.rich, 110)) &&
              message_rich_store_find(f.rich, 110)->message_revision == record(110)->revision,
          "live_voice_partial_attachment_update_does_not_treat_omitted_flags_as_clear");
}
} // namespace maicord
