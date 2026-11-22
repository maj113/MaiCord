#pragma once

#include "maicord/app/chat/messages/timeline.hpp"
#include "maicord/model/chat/message_rich_store.hpp"
#include "maicord/ui/chat/search/scene.hpp"

namespace maicord {

// The rich timeline the client publishes with each search page: one message per result, made from the id,
// channel, author and body of the row the source resolves at that index.
struct SearchRows {
    static constexpr u32 kMaximum = 32;
    MessageRecord records[kMaximum];
    u32 order[kMaximum];
    u8 text[8192];
    MessageRichRecord rich_records[kMaximum];
    u16 rich_order[kMaximum];
    u8 rich_text[1024];
    MessageHandle handles[kMaximum];
    u32 heights[kMaximum];
    u64 fenwick[kMaximum];
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
};

// Gives `source` (context, count and resolve already set) its rich timeline and result order.
inline bool search_rows_publish(SearchRows* rows, UiSearchSource* source) noexcept {
    constexpr u32 capacity = SearchRows::kMaximum;
    const u32 count = source->count;
    if (count > capacity ||
        !message_store_initialize(&rows->store, {rows->records, capacity}, {rows->order, capacity},
                                  {rows->text, sizeof(rows->text)}) ||
        !message_rich_store_initialize(&rows->rich, {rows->rich_records, capacity}, {rows->rich_order, capacity},
                                       {rows->rich_text, sizeof(rows->rich_text)}))
        return false;
    for (u32 i = 0; i < count; ++i) {
        UiSearchRow row{};
        if (!source->resolve(source->context, i, &row))
            return false;
        const auto inserted = message_store_upsert(
            &rows->store, {row.message_id, row.channel_id, row.author_id, 1'700'000'000'000ull + i * 60'000ull, 0,
                           row.body, 0});
        if (!inserted.ok())
            return false;
        rows->handles[i] = inserted.handle;
    }
    if (!ui_variable_list_initialize(&rows->list, {rows->heights, capacity}, {rows->fenwick, capacity}, count,
                                     ui_extent_units(96)) ||
        !message_timeline_initialize(&rows->timeline, &rows->store, &rows->list, ui_extent_units(96),
                                     ui_extent_units(800), false) ||
        !message_timeline_bind_rich_store(&rows->timeline, &rows->rich))
        return false;
    source->rich_timeline = &rows->timeline;
    source->rich_order = {rows->handles, count};
    return true;
}

} // namespace maicord
