#pragma once
#include "application_state.hpp"
#include "application_search.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "account_ui_runtime.hpp"

namespace maicord::app_detail {
// The channel's own readability: a visible, message-capable text surface the account can view and
// read, in an available server (or a direct message). `channel` comes from the store's lookups.
inline bool application_channel_record_readable(const ChatStateStore& state, const ChannelRecord& channel) noexcept {
    constexpr u32 required = channel_action_view | channel_action_read_history;
    if (!channel.occupied || !channel_is_message_capable(channel.type) ||
        (channel.flags & kChannelObfuscatedFlag) || (channel.actions & required) != required ||
        !chat_state_store_channel_visible(state, channel)) return false;
    if (!channel.guild_id) return channel.type == 1 || channel.type == 3;
    const auto* guild = chat_state_store_find_guild(state, channel.guild_id);
    return guild && guild->occupied && !guild->unavailable && channel.permissions_resolved;
}
// The containing message owns access, including forwarded snapshots. A CDN
// origin channel is a URL identity, never a second content permission scope.
inline bool application_channel_readable(const Application& application, u64 channel_id) noexcept {
    if (!application.account_ui || !channel_id) return false;
    const auto& state = application.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, channel_id);
    return channel && application_channel_record_readable(state, *channel);
}
inline bool application_rich_url_owned(const MessageRichStore& store, const MessageRichRecord* rich, Utf8View url) noexcept {
    if (!rich) return false;
    for (u32 i = 0; i < rich->attachment_count; ++i)
        if (utf8_equal(message_rich_store_string(store, rich->attachments[i].url), url)) return true;
    for (u32 i = 0; i < rich->embed_count; ++i) {
        const auto& embed = rich->embeds[i];
        const MessageRichEmbedMedia* media[]{&embed.image, &embed.thumbnail, &embed.video};
        for (const auto* item : media)
            if (utf8_equal(message_rich_store_string(store, item->url), url) ||
                utf8_equal(message_rich_store_string(store, item->proxy_url), url)) return true;
    }
    return false;
}
inline bool application_text_contains(Utf8View text, Utf8View url) noexcept {
    for (u32 at = 0; url.bytes && at + url.bytes <= text.bytes; ++at)
        if (text.data[at] == url.data[0] && bytes_equal(text.data + at, url.data, url.bytes)) return true;
    return false;
}
// Refresh responses share URLs, so authorize them through a current containing
// source (an attachment, embed, or for an attachment chip the message text)
// rather than treating the CDN origin as a permission source.
inline bool application_media_url_authorized(const Application& application, Utf8View url) noexcept {
    if (!application.account_ui || !url.bytes) return false;
    const auto& store = application.message_view.store;
    const auto& rich_store = application.account_ui->message_rich_store;
    for (u32 i = 0; i < store.count; ++i) {
        const auto handle = message_store_ordered_handle(store, i);
        const auto* message = message_store_resolve(store, handle);
        if (!message || message->channel_id != application.account_ui->active_channel_id ||
            !application_channel_readable(application, message->channel_id)) continue;
        const auto* rich = message_rich_store_find(rich_store, message->id);
        if (application_text_contains(message_store_content(store, handle), url)) return true;
        if (rich && rich->message_revision == message->revision &&
            (application_rich_url_owned(rich_store, rich, url) ||
             application_rich_url_owned(rich_store, message_rich_store_snapshot(rich_store, message->id), url))) return true;
    }
    const auto* scene = application_search_scene();
    if (!scene || !scene->source) return false;
    for (u32 i = 0; i < scene->source->count; ++i) {
        ApplicationSearchSourceTicket ticket{};
        if (!application_search_source_ticket(i, &ticket)) continue;
        for (u32 snapshot = 0; snapshot < 2; ++snapshot) {
            ApplicationSearchSourceView view{};
            if (application_search_source_resolve(ticket, snapshot != 0, &view) && view.rich_store &&
                application_rich_url_owned(*view.rich_store, view.rich, url)) return true;
        }
    }
    return false;
}
}
