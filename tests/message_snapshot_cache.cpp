#include "maicord/app/message_snapshot_cache.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {

inline constexpr u32 kRows = 256;
inline constexpr u64 kChannel = 7;
inline constexpr u64 kSelf = 11;
inline constexpr u32 kPageLimit = 25; // kMessageHistoryRequestLimit

struct SnapshotTimeline {
    MessageRecord records[kRows];
    u32 order[kRows];
    u32 heights[kRows];
    u64 sums[kRows];
    u16 rich_order[kRows];
    u8 text[16 * 1024];
    u8 rich_text[256 * 1024];
    StableStorage rich_storage;
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
};

struct SnapshotPage {
    u32 page_structurals[32768];
    u32 message_structurals[16384];
    u8 body[96 * 1024];
    char text[96 * 1024];
    GatewayMessageEvent messages[100];
    GatewayMessageNamedUser mentions[256];
    GatewayMessageReaction reactions[256];
    GatewayMessageAttachment attachments[256];
    GatewayMessageEmbed embeds[128];
    GatewayMessageEmbedField fields[512];
    GatewayMessageSticker stickers[32];
    GatewayMessageComponent components[256];
    GatewayMessageAttachment child_attachments[128];
    GatewayMessageEmbed child_embeds[64];
    GatewayMessageEmbedField child_fields[128];
    GatewayMessageSticker child_stickers[16];
    GatewayMessageComponent child_components[64];
    GatewayMessageNamedUser child_mentions[64];
};

SnapshotTimeline g_live;
SnapshotTimeline g_fresh;
SnapshotPage g_page;
MessageSnapshotCache g_cache;

inline constexpr u32 kUnit = 60 * kUiExtentUnitsPerDip;

bool initialize(SnapshotTimeline& t) noexcept {
    message_rich_store_release(&t.rich);
    return message_store_initialize(&t.store, {t.records, kRows}, {t.order, kRows}, {t.text, sizeof(t.text)}) &&
           message_rich_store_initialize_owned(&t.rich, &t.rich_storage, kRows, {t.rich_order, kRows},
                                               {t.rich_text, sizeof(t.rich_text)}) &&
           ui_variable_list_initialize(&t.list, {t.heights, kRows}, {t.sums, kRows}, 0, kUnit) &&
           message_timeline_initialize(&t.timeline, &t.store, &t.list, kUnit, ui_extent_units(600.0f), true) &&
           message_timeline_bind_rich_store(&t.timeline, &t.rich);
}

bool append(ProtocolWriter* writer, const char* text) noexcept {
    return protocol_writer_append_c_string(writer, text);
}

bool append_id(ProtocolWriter* writer, const char* prefix, u64 value, const char* suffix) noexcept {
    return append(writer, prefix) && protocol_writer_append_u64(writer, value) && append(writer, suffix);
}

bool write_message(ProtocolWriter* w, u64 id, u64 channel, u32 n) noexcept {
    bool ok = append_id(w, "{\"id\":\"", id, "\"") && append_id(w, ",\"channel_id\":\"", channel, "\"") &&
              append_id(w, ",\"author\":{\"id\":\"", 11 + n % 3, "\",\"username\":\"user\"}") &&
              append_id(w, ",\"content\":\"message ", n, " with some ordinary chat text\"") &&
              append(w, ",\"timestamp\":\"2026-01-01T00:00:00Z\"");
    if (n % 3 == 0)
        ok = ok && append_id(w, ",\"attachments\":[{\"id\":\"", id * 10 + 1, "\",\"filename\":\"image.png\"") &&
             append_id(w, ",\"size\":1024,\"url\":\"https://cdn.discordapp.com/attachments/7/", id,
                       "/image.png\",\"width\":640,\"height\":480,\"content_type\":\"image/png\"}]");
    if (n % 4 == 0)
        ok = ok && append(w, ",\"reactions\":[{\"count\":3,\"count_details\":{\"normal\":3,\"burst\":0},\"me\":true,"
                             "\"emoji\":{\"id\":null,\"name\":\"ok\"}},{\"count\":1,\"me\":false,"
                             "\"emoji\":{\"id\":\"44\",\"name\":\"party\",\"animated\":true}}]");
    if (n % 5 == 0)
        ok = ok && append(w, ",\"embeds\":[{\"type\":\"rich\",\"title\":\"Embed title\",\"description\":\"Embed "
                             "description text\",\"url\":\"https://example.com/\",\"image\":{\"url\":"
                             "\"https://example.com/image.png\",\"width\":320,\"height\":200},\"fields\":"
                             "[{\"name\":\"key\",\"value\":\"value\"}]}]");
    if (n % 7 == 3)
        ok = ok && append_id(w, ",\"message_reference\":{\"message_id\":\"", id - 1, "\",\"channel_id\":\"7\"}") &&
             append_id(w, ",\"referenced_message\":{\"id\":\"", id - 1,
                       "\",\"channel_id\":\"7\",\"author\":{\"id\":\"12\",\"username\":\"other\"},"
                       "\"content\":\"quoted text\"}");
    else if (n % 11 == 5)
        ok = ok && append(w, ",\"message_reference\":{\"type\":1,\"channel_id\":\"9\",\"message_id\":\"100\"},"
                             "\"message_snapshots\":[{\"message\":{\"content\":\"forwarded body\",\"timestamp\":"
                             "\"2025-01-01T00:00:00Z\",\"attachments\":[{\"id\":\"77\",\"filename\":\"f.png\","
                             "\"size\":64,\"url\":\"https://cdn.discordapp.com/attachments/9/77/f.png\"}]}}]");
    return ok && append(w, "}");
}

// Newest-first page of count messages ending at newest.
ConstSlice<GatewayMessageEvent> decode_page(u64 newest, u32 count, u64 channel = kChannel) noexcept {
    ProtocolWriter writer{{g_page.body, sizeof(g_page.body)}, 0};
    bool ok = append(&writer, "[");
    for (u32 i = 0; ok && i < count; ++i)
        ok = (!i || append(&writer, ",")) && write_message(&writer, newest - i, channel, static_cast<u32>(newest - i));
    if (!ok || !append(&writer, "]"))
        return {};
    const MessageHistoryRichScratch rich{
        {g_page.reactions, 256}, {g_page.attachments, 256}, {g_page.embeds, 128}, {g_page.fields, 512},
        {g_page.stickers, 32}, {g_page.components, 256}, {},
        {{g_page.child_attachments, 128}, {g_page.child_embeds, 64}, {g_page.child_fields, 128},
         {g_page.child_stickers, 16}, {g_page.child_components, 64}, {g_page.child_mentions, 64}}, {}};
    const auto decoded = decode_message_history_page(
        {reinterpret_cast<const char*>(g_page.body), writer.used}, {g_page.page_structurals, 32768},
        {g_page.message_structurals, 16384}, 64, {g_page.text, sizeof(g_page.text)}, {g_page.messages, 100},
        {g_page.mentions, 256}, rich);
    return decoded.ok() ? ConstSlice<GatewayMessageEvent>{g_page.messages, decoded.page.count}
                        : ConstSlice<GatewayMessageEvent>{};
}

bool load(SnapshotTimeline& t, u64 newest, u32 count) noexcept {
    const auto page = decode_page(newest, count);
    return page.count == count && initialize(t) &&
           message_timeline_apply_history_page(&t.timeline, page, MessageHistoryDirection::newer,
                                               MessageContentAvailability::available)
               .ok();
}

const MessageRecord* at(const SnapshotTimeline& t, u32 index) noexcept {
    return message_store_resolve(t.store, message_store_ordered_handle(t.store, index));
}

// Packed rich data is independent of slots, text banks and revision bindings.
bool same_rich(const SnapshotTimeline& left, const SnapshotTimeline& right, u64 id) noexcept {
    static u8 a[64 * 1024];
    static u8 b[64 * 1024];
    u32 left_bytes = 0;
    u32 right_bytes = 0;
    return message_rich_store_pack(left.rich, id, {a, sizeof(a)}, &left_bytes) &&
           message_rich_store_pack(right.rich, id, {b, sizeof(b)}, &right_bytes) && left_bytes == right_bytes &&
           bytes_equal(a, b, left_bytes);
}

// Rows the scene renders: base records, content and every packed rich field.
bool same_rows(const SnapshotTimeline& left, const SnapshotTimeline& right) noexcept {
    if (left.store.count != right.store.count || left.list.count != left.store.count)
        return false;
    for (u32 i = 0; i < left.store.count; ++i) {
        const MessageRecord* l = at(left, i);
        const MessageRecord* r = at(right, i);
        const Utf8View lc = message_store_content(left.store, message_store_ordered_handle(left.store, i));
        const Utf8View rc = message_store_content(right.store, message_store_ordered_handle(right.store, i));
        const auto* rich = message_rich_store_find(left.rich, l->id);
        if (!l || !r || l->id != r->id || l->channel_id != r->channel_id || l->author_id != r->author_id ||
            l->timestamp_ms != r->timestamp_ms || l->edited_timestamp_ms != r->edited_timestamp_ms ||
            l->flags != r->flags || l->type != r->type || l->content_availability != r->content_availability ||
            l->presentation_flags != r->presentation_flags || l->delivery != r->delivery || lc.bytes != rc.bytes ||
            !bytes_equal(lc.data, rc.data, lc.bytes) || !same_rich(left, right, l->id) ||
            (rich && rich->message_revision != l->revision))
            return false;
    }
    return true;
}

GatewayClientEvent dispatch(GatewayClientDispatchKind kind) noexcept {
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = kind;
    return event;
}

GatewayClientEvent content_update(u64 id, Utf8View content) noexcept {
    GatewayClientEvent event = dispatch(GatewayClientDispatchKind::message_update);
    event.dispatch_payload_bytes = content.bytes;
    event.message.id = id;
    event.message.channel_id = kChannel;
    event.message.content_bytes = content.bytes;
    event.message.fields = gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_content;
    return event;
}

GatewayClientEvent reaction_add(u64 id, Utf8View name, u64 user) noexcept {
    GatewayClientEvent event = dispatch(GatewayClientDispatchKind::message_reaction_add);
    event.dispatch_payload_bytes = name.bytes;
    event.reaction.message_id = id;
    event.reaction.channel_id = kChannel;
    event.reaction.user_id = user;
    event.reaction.name_bytes = static_cast<u16>(name.bytes);
    return event;
}

ConstSlice<u8> bytes_of(Utf8View text) noexcept {
    return {reinterpret_cast<const u8*>(text.data), text.bytes};
}

MessageSnapshotCaptureInput live_input(u64 channel = kChannel) noexcept {
    return {channel, 0, false, true, false};
}

// Raw REST page body decoded through the shipping rich history decoder.
ConstSlice<GatewayMessageEvent> decode_body(Utf8View body, u32* degraded) noexcept {
    const MessageHistoryRichScratch rich{
        {g_page.reactions, 256}, {g_page.attachments, 256}, {g_page.embeds, 128}, {g_page.fields, 512},
        {g_page.stickers, 32}, {g_page.components, 256}, {},
        {{g_page.child_attachments, 128}, {g_page.child_embeds, 64}, {g_page.child_fields, 128},
         {g_page.child_stickers, 16}, {g_page.child_components, 64}, {g_page.child_mentions, 64}}, {}};
    const auto decoded = decode_message_history_page(
        body, {g_page.page_structurals, 32768}, {g_page.message_structurals, 16384}, 64,
        {g_page.text, sizeof(g_page.text)}, {g_page.messages, 100}, {g_page.mentions, 256}, rich);
    if (degraded)
        *degraded = decoded.degraded_messages;
    return decoded.ok() ? ConstSlice<GatewayMessageEvent>{g_page.messages, decoded.page.count}
                        : ConstSlice<GatewayMessageEvent>{};
}

#define THREAD_AUTHOR R"("author":{"id":"12","username":"starter","global_name":"Starter","avatar":null,"discriminator":"0","public_flags":0})"
// A parent channel page as the API returns it once a thread exists: the
// THREAD_CREATED system message (type 18), the starter message carrying the
// full thread channel object (metadata, current-user member, counters, tags),
// an ordinary message, an explicit thread:null and a reply.
constexpr const char kThreadParentPage[] =
    "[{\"type\":18,\"content\":\"Release planning\",\"mentions\":[],\"mention_roles\":[],\"attachments\":[],"
    "\"embeds\":[],\"timestamp\":\"2026-09-29T11:33:10.000000+00:00\",\"edited_timestamp\":null,\"flags\":0,"
    "\"components\":[],\"id\":\"960\",\"channel_id\":\"7\"," THREAD_AUTHOR ",\"pinned\":false,"
    "\"mention_everyone\":false,\"tts\":false,\"message_reference\":{\"type\":0,\"channel_id\":\"950\","
    "\"guild_id\":\"1\"},\"position\":0},"
    "{\"type\":0,\"content\":\"Kicking off the release thread\",\"mentions\":[],\"mention_roles\":[],"
    "\"attachments\":[],\"embeds\":[],\"timestamp\":\"2026-09-29T11:32:52.716000+00:00\","
    "\"edited_timestamp\":null,\"flags\":32,\"components\":[],\"id\":\"950\",\"channel_id\":\"7\","
    THREAD_AUTHOR ",\"pinned\":false,\"mention_everyone\":false,\"tts\":false,"
    "\"thread\":{\"id\":\"950\",\"type\":11,\"last_message_id\":\"952\",\"flags\":0,\"guild_id\":\"1\","
    "\"name\":\"Release planning\",\"parent_id\":\"7\",\"rate_limit_per_user\":0,\"bitrate\":64000,"
    "\"user_limit\":0,\"rtc_region\":null,\"owner_id\":\"12\",\"thread_metadata\":{\"archived\":false,"
    "\"archive_timestamp\":\"2026-09-29T11:32:52.716000+00:00\",\"auto_archive_duration\":4320,"
    "\"locked\":false,\"create_timestamp\":\"2026-09-29T11:32:52.716000+00:00\",\"invitable\":true},"
    "\"message_count\":2,\"member_count\":2,\"total_message_sent\":2,\"member_ids_preview\":[\"12\",\"11\"],"
    "\"applied_tags\":[],\"member\":{\"id\":\"950\",\"user_id\":\"11\",\"join_timestamp\":"
    "\"2026-09-29T11:32:53.000000+00:00\",\"flags\":1,\"muted\":false,\"mute_config\":null}}},"
    "{\"type\":0,\"content\":\"ordinary\",\"id\":\"940\",\"channel_id\":\"7\"," THREAD_AUTHOR ","
    "\"timestamp\":\"2026-09-29T11:30:00.000000+00:00\",\"edited_timestamp\":null,\"flags\":0},"
    "{\"type\":0,\"content\":\"thread removed\",\"id\":\"930\",\"channel_id\":\"7\"," THREAD_AUTHOR ","
    "\"timestamp\":\"2026-09-29T11:29:00.000000+00:00\",\"thread\":null},"
    "{\"type\":19,\"content\":\"a reply\",\"id\":\"920\",\"channel_id\":\"7\"," THREAD_AUTHOR ","
    "\"timestamp\":\"2026-09-29T11:28:00.000000+00:00\",\"message_reference\":{\"type\":0,"
    "\"message_id\":\"910\",\"channel_id\":\"7\",\"guild_id\":\"1\"},\"referenced_message\":null}]";
// The thread's own first page: THREAD_STARTER_MESSAGE (type 21) quoting the
// parent-channel starter, and a forum-style first post whose id is the thread id.
constexpr const char kThreadOwnPage[] =
    "[{\"type\":0,\"content\":\"first reply\",\"id\":\"952\",\"channel_id\":\"950\"," THREAD_AUTHOR ","
    "\"timestamp\":\"2026-09-29T11:34:00.000000+00:00\",\"position\":1},"
    "{\"type\":21,\"content\":\"\",\"id\":\"951\",\"channel_id\":\"950\"," THREAD_AUTHOR ","
    "\"timestamp\":\"2026-09-29T11:32:53.000000+00:00\",\"position\":0,\"message_reference\":{\"type\":0,"
    "\"message_id\":\"950\",\"channel_id\":\"7\",\"guild_id\":\"1\"},\"referenced_message\":{\"type\":0,"
    "\"content\":\"Kicking off the release thread\",\"id\":\"950\",\"channel_id\":\"7\"," THREAD_AUTHOR ","
    "\"timestamp\":\"2026-09-29T11:32:52.716000+00:00\",\"flags\":32}},"
    "{\"type\":0,\"content\":\"forum first post\",\"id\":\"950\",\"channel_id\":\"950\"," THREAD_AUTHOR ","
    "\"timestamp\":\"2026-09-29T11:32:52.716000+00:00\",\"position\":0,\"flags\":0,\"applied_tags\":[\"3\"]}]";
// One message per unexpected nested shape. Each degrades to its core text.
constexpr const char kUnexpectedRichPage[] =
    "[{\"content\":\"thread name is a number\",\"id\":\"990\",\"channel_id\":\"7\"," THREAD_AUTHOR ","
    "\"timestamp\":\"2026-09-29T12:00:00Z\",\"thread\":{\"id\":\"5\",\"name\":7}},"
    "{\"content\":\"reactions is an object\",\"id\":\"980\",\"channel_id\":\"7\"," THREAD_AUTHOR ","
    "\"timestamp\":\"2026-09-29T11:59:00Z\",\"reactions\":{\"count\":1}},"
    "{\"content\":\"fine\",\"id\":\"970\",\"channel_id\":\"7\"," THREAD_AUTHOR ","
    "\"timestamp\":\"2026-09-29T11:58:00Z\"}]";
#undef THREAD_AUTHOR

void test_thread_history_pages(void (*check)(bool, const char*) noexcept) noexcept {
    u32 degraded = ~0u;
    auto page = decode_body({kThreadParentPage, sizeof(kThreadParentPage) - 1}, &degraded);
    check(page.count == 5 && degraded == 0 && (page[1].fields & gateway_message_field_thread) &&
              page[1].thread.present && page[1].thread.id == 950 && page[0].type == 18 &&
              (page[3].fields & gateway_message_field_thread) && !page[3].thread.present,
          "thread history: parent page with thread objects and THREAD_CREATED decodes losslessly");
    bool ready = initialize(g_fresh);
    auto applied = ready ? message_timeline_apply_history_page(&g_fresh.timeline, page, MessageHistoryDirection::newer,
                                                               MessageContentAvailability::available)
                         : MessageHistoryApplyResult{};
    const auto* starter = message_rich_store_find(g_fresh.rich, 950);
    check(applied.ok() && applied.inserted == 5 && !applied.skipped && !applied.rich_skipped &&
              g_fresh.store.count == 5 && starter && starter->thread.id == 950 && starter->thread.message_count == 2 &&
              utf8_equal(message_rich_store_string(g_fresh.rich, starter->thread.name),
                         utf8_literal("Release planning")),
          "thread history: a page with a thread starter applies whole and keeps its thread card");
    // Snapshot capture of the same page (hover prefetch) and its restore.
    message_snapshot_cache_initialize(&g_cache, kMessageSnapshotCacheBudgetBytes);
    ready = message_snapshot_cache_capture_page(&g_cache, kChannel, page, nullptr,
                                                MessageContentAvailability::available, kUnit, true) &&
            initialize(g_live);
    const auto restored = ready ? message_snapshot_cache_restore(&g_cache, kChannel, 960, kPageLimit, &g_live.timeline)
                                : MessageSnapshotRestoreResult{};
    const auto* restored_starter = message_rich_store_find(g_live.rich, 950);
    check(restored.status == MessageSnapshotRestoreStatus::restored && restored.count == 5 &&
              same_rows(g_live, g_fresh) && restored_starter && restored_starter->thread.id == 950,
          "thread history: the prefetched parent page snapshots and restores with its thread card");
    message_snapshot_cache_release(&g_cache);

    page = decode_body({kThreadOwnPage, sizeof(kThreadOwnPage) - 1}, &degraded);
    ready = page.count == 3 && degraded == 0 && page[1].type == 21 && initialize(g_fresh);
    applied = ready ? message_timeline_apply_history_page(&g_fresh.timeline, page, MessageHistoryDirection::newer,
                                                          MessageContentAvailability::available)
                    : MessageHistoryApplyResult{};
    check(applied.ok() && applied.inserted == 3 && !applied.skipped && g_fresh.store.count == 3,
          "thread history: THREAD_STARTER_MESSAGE and forum first post load in the thread itself");

    page = decode_body({kUnexpectedRichPage, sizeof(kUnexpectedRichPage) - 1}, &degraded);
    ready = page.count == 3 && degraded == 2 && !(page[0].fields & gateway_message_field_thread) &&
            !(page[1].fields & gateway_message_field_reactions) &&
            utf8_equal(page[0].content, utf8_literal("thread name is a number")) && initialize(g_fresh);
    applied = ready ? message_timeline_apply_history_page(&g_fresh.timeline, page, MessageHistoryDirection::newer,
                                                          MessageContentAvailability::available)
                    : MessageHistoryApplyResult{};
    check(applied.ok() && applied.inserted == 3 && g_fresh.store.count == 3,
          "thread history: one unexpected nested field degrades that message, never the page");

    // A message the store cannot admit is left out; the rest of the page loads.
    constexpr u32 create = gateway_message_field_id | gateway_message_field_channel_id |
                           gateway_message_field_author_id | gateway_message_field_timestamp |
                           gateway_message_field_content;
    const GatewayMessageEvent mixed[3]{
        {1003, kChannel, 12, 1003, 0, utf8_literal("kept newest"), 0, create},
        {1002, kChannel, 12, 1002, 0, utf8_literal("no author field"), 0,
         create & ~static_cast<u32>(gateway_message_field_author_id)},
        {1001, kChannel, 12, 1001, 0, utf8_literal("kept oldest"), 0, create},
    };
    ready = initialize(g_fresh);
    applied = ready ? message_timeline_apply_history_page(&g_fresh.timeline, {mixed, 3}, MessageHistoryDirection::newer,
                                                          MessageContentAvailability::available)
                    : MessageHistoryApplyResult{};
    check(applied.ok() && applied.inserted == 2 && applied.skipped == 1 && g_fresh.store.count == 2 &&
              message_store_find(g_fresh.store, 1003).ok() && message_store_find(g_fresh.store, 1001).ok() &&
              !message_store_find(g_fresh.store, 1002).ok(),
          "thread history: an inadmissible message is skipped instead of failing the channel");
    const GatewayMessageEvent* skipped_only = &mixed[1];
    ready = initialize(g_fresh);
    check(ready && !message_snapshot_cache_capture_page(&g_cache, kChannel, {skipped_only, 1}, nullptr,
                                                        MessageContentAvailability::available, kUnit, true),
          "thread history: a page history apply would degrade is not snapshotted");
    message_snapshot_cache_release(&g_cache);
}

} // namespace

void test_message_snapshot_cache(void (*check)(bool, const char*) noexcept) noexcept {
    message_snapshot_cache_initialize(&g_cache, kMessageSnapshotCacheBudgetBytes);
    constexpr u64 kNewest = 1000;
    constexpr u32 kCount = 80;

    // Capture/restore equivalence against a fresh load of the same page.
    bool ready = load(g_live, kNewest, kCount) && load(g_fresh, kNewest, kCount);
    MessageTimelineInvalidation invalidation{};
    for (u32 i = 0; ready && i < kCount; i += 3)
        ready = message_timeline_set_item_height(&g_live.timeline, i, kUnit + ui_extent_units(7.0f * (i % 5)),
                                                 &invalidation);
    ready = ready && message_timeline_set_scroll(&g_live.timeline, ui_variable_list_prefix_units(g_live.list, 30) +
                                                                       ui_extent_units(13.0f),
                                                 false);
    u32 heights[kRows]{};
    for (u32 i = 0; i < g_live.list.count; ++i)
        heights[i] = ui_variable_list_item_units(g_live.list, i);
    const u64 scroll = g_live.timeline.scroll_units;
    check(ready && same_rows(g_live, g_fresh), "snapshot fixture: decoded rich page loads identically twice");
    const bool captured = message_snapshot_cache_capture(&g_cache, g_live.timeline, live_input());
    const auto* entry = message_snapshot_cache_find(g_cache, kChannel);
    check(captured && entry && entry->count == kCount && entry->newest_id == kNewest && !entry->follow_bottom &&
              entry->bytes < 64 * 1024,
          "snapshot capture packs the window into a bounded entry");
    const u32 reset_generation = g_live.timeline.reset_generation;
    auto restored = MessageSnapshotRestoreResult{};
    ready = message_timeline_reset(&g_live.timeline);
    if (ready)
        restored = message_snapshot_cache_restore(&g_cache, kChannel, kNewest, kPageLimit,
                                                  &g_live.timeline);
    bool heights_kept = restored.status == MessageSnapshotRestoreStatus::restored;
    for (u32 i = 0; heights_kept && i < kCount; ++i)
        heights_kept = ui_variable_list_item_units(g_live.list, i) == heights[i];
    check(ready && restored.status == MessageSnapshotRestoreStatus::restored && restored.count == kCount &&
              same_rows(g_live, g_fresh) && g_live.timeline.reset_generation == reset_generation + 1,
          "snapshot restore renders rows identical to a fresh load");
    check(heights_kept && g_live.timeline.scroll_units == scroll && !g_live.timeline.follow_bottom &&
              !message_snapshot_cache_find(g_cache, kChannel) && g_cache.used == 0 && !g_cache.pool.base,
          "snapshot restore keeps measurements and anchor, and consumes the entry");

    // Local outbox rows are re-projected by their owner, never retained.
    MessageInput local{5000, kChannel, kSelf, 0, 0, utf8_literal("pending"), 0};
    local.delivery = MessageDeliveryState::pending;
    ready = message_timeline_set_scroll(&g_live.timeline, 0, true) &&
            message_timeline_insert_local(&g_live.timeline, local).ok() &&
            message_snapshot_cache_capture(&g_cache, g_live.timeline, live_input()) &&
            message_timeline_reset(&g_live.timeline);
    restored = message_snapshot_cache_restore(&g_cache, kChannel, kNewest, kPageLimit,
                                              &g_live.timeline);
    check(ready && restored.status == MessageSnapshotRestoreStatus::restored &&
              !message_store_find(g_live.store, 5000).ok() && same_rows(g_live, g_fresh) &&
              g_live.timeline.follow_bottom,
          "snapshot capture omits local outbox rows and restores bottom follow");

    // Gateway events applied to the cached snapshot match the live path.
    ready = message_snapshot_cache_capture(&g_cache, g_live.timeline, live_input());
    u64 bulk_ids[4]{};
    const auto edited = utf8_literal("edited while cached");
    const auto apply_both = [&](const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
        message_snapshot_cache_observe(&g_cache, event, payload, {bulk_ids, 4}, kSelf,
                                       MessageContentAvailability::available);
        return message_timeline_apply(&g_fresh.timeline, event, payload, {bulk_ids, 4},
                                      MessageContentAvailability::available)
            .ok();
    };
    ready = ready && apply_both(content_update(kNewest - 4, edited), bytes_of(edited));
    const auto heart = utf8_literal("heart");
    const auto added = reaction_add(kNewest - 8, heart, kSelf);
    message_snapshot_cache_observe(&g_cache, added, bytes_of(heart), {bulk_ids, 4}, kSelf,
                                   MessageContentAvailability::available);
    const auto* target = message_store_resolve(g_fresh.store, message_store_find(g_fresh.store, kNewest - 8).handle);
    ready = ready && target &&
            message_rich_store_mutate_reaction(&g_fresh.rich, kNewest - 8, target->revision,
                                               {0, heart, MessageReactionType::normal,
                                                MessageRichReactionMutationKind::add, true, false})
                .ok();
    auto erased = dispatch(GatewayClientDispatchKind::message_delete);
    erased.message.id = kNewest - 10;
    erased.message.channel_id = kChannel;
    ready = ready && apply_both(erased, {});
    u8 bulk_payload[16]{};
    const u64 bulk[2]{kNewest - 20, kNewest - 21};
    copy_bytes(bulk_payload, bulk, sizeof(bulk));
    auto bulk_event = dispatch(GatewayClientDispatchKind::message_bulk_delete);
    bulk_event.message.channel_id = kChannel;
    bulk_event.message.id_count = 2;
    bulk_event.dispatch_payload_bytes = sizeof(bulk_payload);
    ready = ready && apply_both(bulk_event, {bulk_payload, sizeof(bulk_payload)});
    auto other = content_update(kNewest - 2, edited);
    other.message.channel_id = 99;
    message_snapshot_cache_observe(&g_cache, other, bytes_of(edited), {bulk_ids, 4}, kSelf,
                                   MessageContentAvailability::available);
    auto create = dispatch(GatewayClientDispatchKind::message_create);
    create.message.channel_id = kChannel;
    message_snapshot_cache_observe(&g_cache, create, {}, {bulk_ids, 4}, kSelf, MessageContentAvailability::available);
    entry = message_snapshot_cache_find(g_cache, kChannel);
    check(ready && entry && entry->count == kCount - 3 && entry->creates == 1 && g_cache.event_updates == 2 &&
              !g_cache.event_drops,
          "snapshot follows update, reaction, delete and bulk delete; counts creates");
    ready = message_timeline_reset(&g_live.timeline);
    restored = message_snapshot_cache_restore(&g_cache, kChannel, kNewest, kPageLimit,
                                              &g_live.timeline);
    const auto* reacted = message_rich_store_find(g_live.rich, kNewest - 8);
    bool self_reaction = false;
    for (u32 i = 0; reacted && i < reacted->reaction_count; ++i)
        self_reaction = self_reaction || (utf8_equal(message_rich_store_string(g_live.rich, reacted->reactions[i].name),
                                                     heart) && (reacted->reactions[i].flags & 1u));
    check(ready && restored.status == MessageSnapshotRestoreStatus::restored && same_rows(g_live, g_fresh) &&
              self_reaction,
          "snapshot event application equals live application after restore");

    // Gap detection falls back to the cold path with an empty timeline.
    ready = message_snapshot_cache_capture(&g_cache, g_live.timeline, live_input());
    for (u32 i = 0; i < kPageLimit; ++i)
        message_snapshot_cache_observe(&g_cache, create, {}, {bulk_ids, 4}, kSelf,
                                       MessageContentAvailability::available);
    ready = ready && message_timeline_reset(&g_live.timeline);
    restored = message_snapshot_cache_restore(&g_cache, kChannel, kNewest + 30, kPageLimit,
                                              &g_live.timeline);
    check(ready && restored.status == MessageSnapshotRestoreStatus::gap && !g_live.store.count &&
              !message_snapshot_cache_find(g_cache, kChannel),
          "snapshot restore detects a newer gap larger than one page");
    ready = load(g_live, kNewest, kCount) && message_snapshot_cache_capture(&g_cache, g_live.timeline, live_input()) &&
            message_timeline_reset(&g_live.timeline);
    restored = message_snapshot_cache_restore(&g_cache, kChannel, kNewest + 1, kPageLimit,
                                              &g_live.timeline);
    check(ready && restored.status == MessageSnapshotRestoreStatus::gap && !g_live.store.count,
          "snapshot restore treats an unobserved newer channel tail as a gap");
    ready = load(g_live, kNewest, kCount) && message_snapshot_cache_capture(&g_cache, g_live.timeline, live_input());
    for (u32 i = 0; i < 3; ++i)
        message_snapshot_cache_observe(&g_cache, create, {}, {bulk_ids, 4}, kSelf,
                                       MessageContentAvailability::available);
    ready = ready && message_timeline_reset(&g_live.timeline);
    restored = message_snapshot_cache_restore(&g_cache, kChannel, kNewest + 3, kPageLimit,
                                              &g_live.timeline);
    check(ready && restored.status == MessageSnapshotRestoreStatus::restored && g_live.store.count == kCount,
          "snapshot restore accepts a newer tail within one catch-up page");

    // Permission loss prunes; a non-empty target timeline fails cleanly.
    ready = message_snapshot_cache_capture(&g_cache, g_live.timeline, live_input());
    message_snapshot_cache_prune(&g_cache, [](void*, u64 channel) noexcept { return channel != kChannel; }, nullptr);
    check(ready && !message_snapshot_cache_find(g_cache, kChannel) && !g_cache.pool.base,
          "snapshot prune drops channels that lost read access");
    ready = message_snapshot_cache_capture(&g_cache, g_live.timeline, live_input());
    restored = message_snapshot_cache_restore(&g_cache, kChannel, kNewest, kPageLimit,
                                              &g_live.timeline);
    check(ready && restored.status == MessageSnapshotRestoreStatus::failed && !g_live.store.count &&
              !message_snapshot_cache_find(g_cache, kChannel),
          "snapshot restore into an occupied timeline resets and drops the entry");

    // LRU eviction by entry count and bytes; speculative prefetches go first.
    ready = load(g_live, kNewest, 40);
    for (u64 channel = 1; ready && channel <= kMessageSnapshotCacheEntries; ++channel) {
        for (u32 i = 0; i < g_live.store.count; ++i)
            g_live.records[g_live.order[i]].channel_id = channel;
        ready = message_snapshot_cache_capture(&g_cache, g_live.timeline, live_input(channel));
    }
    const auto page = decode_page(2000, 20, 50);
    ready = ready && page.count == 20 &&
            message_snapshot_cache_capture_page(&g_cache, 50, page, nullptr, MessageContentAvailability::available,
                                                kUnit, true) &&
            !message_snapshot_cache_find(g_cache, 1) && message_snapshot_cache_find(g_cache, 50)->speculative;
    for (u32 i = 0; i < g_live.store.count; ++i)
        g_live.records[g_live.order[i]].channel_id = 60;
    ready = ready && message_snapshot_cache_capture(&g_cache, g_live.timeline, live_input(60));
    check(ready && !message_snapshot_cache_find(g_cache, 50) && message_snapshot_cache_find(g_cache, 2) &&
              message_snapshot_cache_find(g_cache, 60) &&
              message_snapshot_cache_count(g_cache) == kMessageSnapshotCacheEntries,
          "snapshot cache evicts speculative prefetches before shown channels");
    for (u32 i = 0; i < g_live.store.count; ++i)
        g_live.records[g_live.order[i]].channel_id = 61;
    ready = message_snapshot_cache_capture(&g_cache, g_live.timeline, live_input(61));
    check(ready && !message_snapshot_cache_find(g_cache, 1) && !message_snapshot_cache_find(g_cache, 2) &&
              message_snapshot_cache_find(g_cache, 3) && message_snapshot_cache_find(g_cache, 61),
          "snapshot cache evicts the least recently used entry");
    ready = message_timeline_reset(&g_live.timeline);
    restored = message_snapshot_cache_restore(&g_cache, 50, 0, kPageLimit, &g_live.timeline);
    check(ready && restored.status == MessageSnapshotRestoreStatus::missing, "snapshot restore misses evicted entry");

    // A prefetched page restores like the history page it came from.
    const auto prefetched = decode_page(3000, 25, 70);
    ready = prefetched.count == 25 && initialize(g_fresh) &&
            message_timeline_apply_history_page(&g_fresh.timeline, prefetched, MessageHistoryDirection::newer,
                                                MessageContentAvailability::available)
                .ok() &&
            message_snapshot_cache_capture_page(&g_cache, 70, prefetched, nullptr,
                                                MessageContentAvailability::available, kUnit, false) &&
            message_timeline_reset(&g_live.timeline);
    restored = message_snapshot_cache_restore(&g_cache, 70, 3000, kPageLimit, &g_live.timeline);
    check(ready && restored.status == MessageSnapshotRestoreStatus::restored && same_rows(g_live, g_fresh) &&
              g_live.timeline.follow_bottom && !restored.older_exhausted,
          "prefetched page snapshot restores identically to a fresh page load");

    // Account reset drops everything and returns the pool.
    message_snapshot_cache_release(&g_cache);
    check(!message_snapshot_cache_count(g_cache) && !g_cache.pool.base && !g_cache.scratch && !g_cache.used,
          "snapshot release drops every channel on account reset");

    // Prefetch dedupe, rate limit, cancellation and adoption.
    ChannelPrefetch prefetch{};
    channel_prefetch_hover(&prefetch, 50, 1000);
    check(!channel_prefetch_due(prefetch, 1099) && channel_prefetch_due(prefetch, 1100) == 50 &&
              channel_prefetch_next_wake(prefetch) == 1100,
          "prefetch waits for the hover dwell");
    channel_prefetch_submitted(&prefetch, 50, 1, 1100);
    channel_prefetch_hover(&prefetch, 51, 1150);
    check(!channel_prefetch_due(prefetch, 1200) && !channel_prefetch_next_wake(prefetch),
          "prefetch keeps one request in flight");
    check(channel_prefetch_complete(&prefetch, 1, 1300) && !channel_prefetch_complete(&prefetch, 1, 1300),
          "prefetch result is kept shortly after the pointer leaves");
    check(!channel_prefetch_due(prefetch, 1499) && channel_prefetch_due(prefetch, 1500) == 51,
          "prefetch submissions are rate limited");
    channel_prefetch_hover(&prefetch, 50, 1600);
    check(!channel_prefetch_due(prefetch, 9000) && prefetch.deduplicated == 1,
          "prefetch deduplicates a recently requested channel");
    channel_prefetch_hover(&prefetch, 52, 10000);
    channel_prefetch_hover(&prefetch, 0, 10050);
    check(!channel_prefetch_due(prefetch, 20000) && prefetch.cancelled >= 1, "prefetch cancels a short hover");
    channel_prefetch_hover(&prefetch, 53, 30000);
    channel_prefetch_submitted(&prefetch, 53, 2, 30100);
    check(!channel_prefetch_adopt(&prefetch, 2, 54) && channel_prefetch_adopt(&prefetch, 2, 53) &&
              !channel_prefetch_complete(&prefetch, 2, 30200) && prefetch.adopted == 1,
          "prefetch request is adopted by the selected channel once");
    channel_prefetch_hover(&prefetch, 54, 40000);
    channel_prefetch_submitted(&prefetch, 54, 3, 40100);
    channel_prefetch_hover(&prefetch, 0, 40110);
    check(!channel_prefetch_complete(&prefetch, 3, 40110 + kChannelPrefetchKeepMs + 1) && prefetch.discarded == 1,
          "prefetch discards a result after the keep window");

    test_thread_history_pages(check);
    message_rich_store_release(&g_live.rich);
    message_rich_store_release(&g_fresh.rich);
}

} // namespace maicord
