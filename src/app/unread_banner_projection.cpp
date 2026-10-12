#include "maicord/app/unread_banner_projection.hpp"

namespace maicord {
namespace {
bool input_valid(const UnreadBannerInput& in) noexcept {
    return in.store && in.list && in.channel_id && in.viewport_units && in.list->count == in.store->count;
}
u64 maximum_scroll(const UnreadBannerInput& in) noexcept {
    return in.list->total_units > in.viewport_units ? in.list->total_units - in.viewport_units : 0;
}
bool far_from_live_edge(const UnreadBannerInput& in) noexcept {
    const u64 bottom = maximum_scroll(in);
    const u64 distance = bottom > in.scroll_units ? bottom - in.scroll_units : 0;
    return in.newer_detached || distance > in.viewport_units * kUnreadBannerJumpViewports;
}
} // namespace

UnreadBannerProjection unread_banner_project(const UnreadBannerInput& in) noexcept {
    UnreadBannerProjection out{};
    if (!input_valid(in))
        return out;
    out.view.channel_id = in.channel_id;
    out.view.jump = far_from_live_edge(in);
    const bool unread = in.readable && in.read_state_known && in.last_message_id > in.last_read_message_id;
    if (!unread)
        return out;
    u32 count = 0;
    u64 oldest_resident = 0;
    for (u32 i = 0; i < in.store->count; ++i) {
        const auto* record = message_store_resolve(*in.store, message_store_ordered_handle(*in.store, i));
        if (!record || record->channel_id != in.channel_id || record->delivery != MessageDeliveryState::authoritative)
            continue;
        if (!oldest_resident)
            oldest_resident = record->id;
        if (record->id <= in.last_read_message_id)
            continue;
        if (!out.first_unread_id)
            out.first_unread_id = record->id;
        ++count;
    }
    // The cursor predates the resident window: older unread messages exist
    // that were never counted. A detached tail also hides newer ones.
    const bool older_hidden =
        !in.older_exhausted && (!oldest_resident || oldest_resident > in.last_read_message_id);
    out.view.count_estimated = older_hidden || in.newer_detached;
    out.view.count = maximum<u32>(count, 1);
    const u64 since_id = out.first_unread_id && !older_hidden ? out.first_unread_id
                         : in.last_read_message_id   ? in.last_read_message_id + 1
                                                     : 0;
    out.view.since_unix_ms = since_id ? snowflake_timestamp_ms(since_id) : 0;
    // As in the official client, the bar follows unread state rather than scroll position;
    // automatic acknowledgement at the live edge retires it.
    out.view.banner = true;
    return out;
}

UnreadEscapePlan unread_banner_escape_plan(const UnreadBannerInput& in) noexcept {
    if (!input_valid(in))
        return {};
    const bool unread = in.readable && in.read_state_known && in.last_message_id > in.last_read_message_id;
    const u64 bottom = maximum_scroll(in);
    return {unread, in.newer_detached || in.scroll_units < bottom};
}

} // namespace maicord
