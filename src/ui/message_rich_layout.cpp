#include "maicord/ui/message_rich_layout.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/decimal.hpp"
#include "maicord/protocol/embed_provider.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/message_component_layout.hpp"
#include "maicord/ui/audio_controls.hpp"
#include "maicord/ui/theme_style_projection.hpp"

namespace maicord {
namespace {

bool ascii_equal_case(Utf8View value, const char* literal) noexcept {
    u32 length = 0;
    while (literal && literal[length] != 0) {
        ++length;
    }
    if (value.bytes != length) {
        return false;
    }
    for (u32 index = 0; index < length; ++index) {
        const char left = value.data[index] >= 'A' && value.data[index] <= 'Z'
                              ? static_cast<char>(value.data[index] + ('a' - 'A'))
                              : value.data[index];
        const char right = literal[index] >= 'A' && literal[index] <= 'Z'
                               ? static_cast<char>(literal[index] + ('a' - 'A'))
                               : literal[index];
        if (left != right) {
            return false;
        }
    }
    return true;
}

bool ascii_ends_with_case(Utf8View value, const char* suffix) noexcept {
    u32 suffix_bytes = 0;
    while (suffix && suffix[suffix_bytes] != 0) {
        ++suffix_bytes;
    }
    if (suffix_bytes > value.bytes) {
        return false;
    }
    const u32 offset = value.bytes - suffix_bytes;
    for (u32 index = 0; index < suffix_bytes; ++index) {
        const char left = value.data[offset + index] >= 'A' && value.data[offset + index] <= 'Z'
                              ? static_cast<char>(value.data[offset + index] + ('a' - 'A'))
                              : value.data[offset + index];
        const char right = suffix[index] >= 'A' && suffix[index] <= 'Z'
                               ? static_cast<char>(suffix[index] + ('a' - 'A'))
                               : suffix[index];
        if (left != right) {
            return false;
        }
    }
    return true;
}

MediaAttachmentFormat media_format_from_url(Utf8View source) noexcept {
    for (u32 index = 0; index < source.bytes; ++index) {
        if (source.data[index] == '?' || source.data[index] == '#') {
            source.bytes = index;
            break;
        }
    }
    if (ascii_ends_with_case(source, ".png")) {
        return MediaAttachmentFormat::png;
    }
    if (ascii_ends_with_case(source, ".webp")) {
        return MediaAttachmentFormat::webp;
    }
    if (ascii_ends_with_case(source, ".gif")) {
        return MediaAttachmentFormat::gif;
    }
    if (ascii_ends_with_case(source, ".jpg") || ascii_ends_with_case(source, ".jpeg")) {
        return MediaAttachmentFormat::jpeg;
    }
    if (ascii_ends_with_case(source, ".mp4")) {
        return MediaAttachmentFormat::mp4;
    }
    return MediaAttachmentFormat::unknown;
}

bool view_equal(Utf8View left, Utf8View right) noexcept {
    if (left.bytes != right.bytes) {
        return false;
    }
    for (u32 index = 0; index < left.bytes; ++index) {
        if (left.data[index] != right.data[index]) {
            return false;
        }
    }
    return true;
}

bool bounded_media_extent(u32 source_width, u32 source_height, float available_width, float maximum_width,
                          float maximum_height, float* width, float* height) noexcept {
    if (!width || !height || source_width == 0 || source_height == 0 || available_width <= 0.0f ||
        maximum_width <= 0.0f || maximum_height <= 0.0f) {
        return false;
    }
    float preview_width =
        minimum(minimum(available_width, maximum_width), static_cast<float>(source_width));
    float preview_height =
        preview_width * static_cast<float>(source_height) / static_cast<float>(source_width);
    if (preview_height > maximum_height) {
        preview_width *= maximum_height / preview_height;
        preview_height = maximum_height;
    }
    if (!ui_extent_valid(preview_width) || !ui_extent_valid(preview_height) || preview_width <= 0.0f ||
        preview_height <= 0.0f) {
        return false;
    }
    *width = preview_width;
    *height = preview_height;
    return true;
}

u32 embed_field_row_count(const MessageRichRecord& rich, const MessageRichEmbed& embed) noexcept {
    const u32 end = minimum(static_cast<u32>(rich.embed_field_count),
                            static_cast<u32>(embed.field_begin) + static_cast<u32>(embed.field_count));
    u32 rows = 0;
    for (u32 field = embed.field_begin; field < end;) {
        ++rows;
        if (!rich.embed_fields[field].inline_field) {
            ++field;
            continue;
        }
        u32 columns = 0;
        while (field < end && rich.embed_fields[field].inline_field && columns < 3) {
            ++field;
            ++columns;
        }
    }
    return rows;
}

MessageRichDisplay g_display{};

} // namespace

const MessageRichDisplay& message_rich_display() noexcept {
    return g_display;
}
bool message_rich_set_display(const MessageRichDisplay& display) noexcept {
    const bool geometry = display.attachment_media != g_display.attachment_media ||
                          display.embed_media != g_display.embed_media || display.embeds != g_display.embeds ||
                          display.reactions != g_display.reactions;
    g_display = display;
    return geometry;
}

u32 message_thread_card_detail(const MessageRichThread& thread, Slice<char> output) noexcept {
    if (!output.data)
        return 0;
    u32 used = 0;
    bool overflow = false;
    const auto append = [&](const char* text, u32 bytes) noexcept {
        if (bytes > output.count - used) {
            overflow = true;
            return;
        }
        copy_bytes(output.data + used, text, bytes);
        used += bytes;
    };
    const auto separator = [&]() noexcept {
        if (used)
            append(" \xC2\xB7 ", 4);
    };
    const auto count = [&](u32 value, const char* one, u32 one_bytes, const char* many, u32 many_bytes) noexcept {
        const bool singular = value == 1;
        char digits[10];
        const u32 n = decimal_write(digits, value);
        separator();
        append(digits, n);
        append(" ", 1);
        if (singular)
            append(one, one_bytes);
        else
            append(many, many_bytes);
    };
    if (thread.message_count)
        count(thread.message_count, "message", 7, "messages", 8);
    if (thread.member_count)
        count(thread.member_count, "member", 6, "members", 7);
    if (thread.locked) {
        separator();
        append("Locked", 6);
    } else if (thread.archived) {
        separator();
        append("Archived", 8);
    }
    return overflow ? 0 : used;
}

bool message_rich_layout_valid(const MessageRichLayout& layout) noexcept {
    return ui_extent_valid(layout.attachments) && ui_extent_valid(layout.embeds) &&
           ui_extent_valid(layout.stickers) && ui_extent_valid(layout.components) &&
           ui_extent_valid(layout.reactions) && ui_extent_valid(layout.notice) &&
           ui_extent_valid(layout.thread) && ui_extent_valid(layout.invites) && ui_extent_valid(layout.total());
}

bool message_rich_layout_equal(const MessageRichLayout& left, const MessageRichLayout& right) noexcept {
    return left.attachments == right.attachments && left.embeds == right.embeds &&
           left.stickers == right.stickers && left.components == right.components &&
           left.reactions == right.reactions && left.notice == right.notice && left.thread == right.thread &&
           left.invites == right.invites;
}

// Every MessageRichItemLayout member is a float extent.
constexpr u32 kItemLayoutFloats = sizeof(MessageRichItemLayout) / sizeof(float);
static_assert(sizeof(MessageRichItemLayout) == kItemLayoutFloats * sizeof(float));

bool message_rich_item_layout_valid(const MessageRichItemLayout& layout) noexcept {
    const auto* values = reinterpret_cast<const float*>(&layout);
    for (u32 index = 0; index < kItemLayoutFloats; ++index)
        if (!ui_extent_valid(values[index]))
            return false;
    float total = 0.0f;
    for (float height : layout.heights)
        total += height;
    return ui_extent_valid(total);
}

bool message_rich_item_layout_equal(const MessageRichItemLayout& left,
                                    const MessageRichItemLayout& right) noexcept {
    const auto* a = reinterpret_cast<const float*>(&left);
    const auto* b = reinterpret_cast<const float*>(&right);
    for (u32 index = 0; index < kItemLayoutFloats; ++index)
        if (a[index] != b[index])
            return false;
    return true;
}

bool message_rich_item_slot(MessageRichItemKind kind, u32 index, u32* slot) noexcept {
    if (!slot) {
        return false;
    }
    switch (kind) {
    case MessageRichItemKind::attachment:
        if (index < kGatewayMessageAttachmentMaximum) {
            *slot = kMessageRichAttachmentItemOffset + index;
            return true;
        }
        break;
    case MessageRichItemKind::embed:
        if (index < kGatewayMessageEmbedMaximum) {
            *slot = kMessageRichEmbedItemOffset + index;
            return true;
        }
        break;
    case MessageRichItemKind::sticker:
        if (index < kGatewayMessageStickerMaximum) {
            *slot = kMessageRichStickerItemOffset + index;
            return true;
        }
        break;
    case MessageRichItemKind::component:
        if (index < kGatewayMessageComponentMaximum) {
            *slot = kMessageRichComponentItemOffset + index;
            return true;
        }
        break;
    case MessageRichItemKind::notice:
        if (index == 0) {
            *slot = kMessageRichNoticeItemOffset;
            return true;
        }
        break;
    case MessageRichItemKind::invite:
        if (index < kMessageInviteCardMaximum) {
            *slot = kMessageRichInviteItemOffset + index;
            return true;
        }
        break;
    case MessageRichItemKind::poll:
        // Polls have one independent optional row root, not rich-item arrays.
        break;
    }
    return false;
}

MediaAttachmentFormat message_rich_attachment_format(const MessageRichStore& store,
                                                     const MessageRichAttachment& attachment) noexcept {
    const auto audio = message_rich_attachment_audio_format(store, attachment);
    if (media_attachment_format_audio_supported(audio))
        return audio;
    const Utf8View content_type = message_rich_store_string(store, attachment.content_type);
    const Utf8View filename = message_rich_store_string(store, attachment.filename);
    if (ascii_equal_case(content_type, "image/png") || ascii_ends_with_case(filename, ".png")) {
        return MediaAttachmentFormat::png;
    }
    if (ascii_equal_case(content_type, "image/webp") || ascii_ends_with_case(filename, ".webp")) {
        return MediaAttachmentFormat::webp;
    }
    if (ascii_equal_case(content_type, "image/gif") || ascii_ends_with_case(filename, ".gif")) {
        return MediaAttachmentFormat::gif;
    }
    if (ascii_equal_case(content_type, "image/jpeg") || ascii_ends_with_case(filename, ".jpg") ||
        ascii_ends_with_case(filename, ".jpeg")) {
        return MediaAttachmentFormat::jpeg;
    }
    // QuickTime movies use the same box structure and H.264/HEVC/AAC tracks.
    if (ascii_equal_case(content_type, "video/mp4") || ascii_ends_with_case(filename, ".mp4") ||
        ascii_equal_case(content_type, "video/quicktime") || ascii_ends_with_case(filename, ".mov")) {
        return MediaAttachmentFormat::mp4;
    }
    return MediaAttachmentFormat::unknown;
}

bool message_rich_attachment_preview_extent(const MessageRichStore& store,
                                            const MessageRichAttachment& attachment, float available_width,
                                            float* width, float* height) noexcept {
    if (!width || !height || available_width <= 0.0f) {
        return false;
    }
    const auto format = message_rich_attachment_format(store, attachment);
    if (format == MediaAttachmentFormat::unknown || media_attachment_format_audio_supported(format) ||
        !g_display.attachment_media) {
        return false;
    }
    // A pending server media probe may have no dimensions yet. Reserve a
    // preview so the decoder can run and publish the actual aspect ratio.
    if ((attachment.state & 3u) != 3u || !attachment.width || !attachment.height)
        return bounded_media_extent(400, 225, available_width, 550.0f, 350.0f, width, height);
    // Ordinary images are bounded to 550x350 and keep their aspect ratio; the
    // available message width is the tighter horizontal limit on compact windows.
    return bounded_media_extent(attachment.width, attachment.height, available_width, 550.0f, 350.0f, width,
                                height);
}

MediaAttachmentFormat embed_media_format(const MessageRichStore& store,
                                         const MessageRichEmbedMedia& media) noexcept {
    const Utf8View content_type = message_rich_store_string(store, media.content_type);
    const Utf8View original = message_rich_store_string(store, media.url);
    if (ascii_equal_case(content_type, "image/png")) {
        return MediaAttachmentFormat::png;
    }
    if (ascii_equal_case(content_type, "image/webp")) {
        return MediaAttachmentFormat::webp;
    }
    if (ascii_equal_case(content_type, "image/gif")) {
        return MediaAttachmentFormat::gif;
    }
    if (ascii_equal_case(content_type, "image/jpeg") || ascii_equal_case(content_type, "image/jpg")) {
        return MediaAttachmentFormat::jpeg;
    }
    if (ascii_equal_case(content_type, "video/mp4") || ascii_equal_case(content_type, "video/quicktime")) {
        return MediaAttachmentFormat::mp4;
    }
    // A media proxy URL for external media may omit the resource's extension.
    // Keep the proxy as the fetch source, but infer its decoder from the
    // canonical embed URL before rejecting the media.
    const MediaAttachmentFormat proxy_format =
        media_format_from_url(message_rich_store_string(store, media.proxy_url));
    return proxy_format != MediaAttachmentFormat::unknown ? proxy_format : media_format_from_url(original);
}

bool select_embed_media_candidate(const MessageRichStore& store, const MessageRichEmbedMedia& media,
                                  MediaEmbedResource resource, bool animated,
                                  MessageRichEmbedMediaSelection* selection) noexcept {
    if (!selection) {
        return false;
    }
    const Utf8View original = message_rich_store_string(store, media.url);
    Utf8View source = message_rich_store_string(store, media.proxy_url);
    if (source.bytes == 0) {
        // A provider's player page is HTML, even when its path or metadata
        // looks like an MP4. Only resolved media enters the native decoder.
        if (resource == MediaEmbedResource::video && embed_provider(original) != EmbedProvider::none)
            return false;
        source = original;
    }
    const MediaAttachmentFormat format = embed_media_format(store, media);
    if (source.bytes == 0 || format == MediaAttachmentFormat::unknown) {
        return false;
    }
    *selection = MessageRichEmbedMediaSelection{
        &media,
        source,
        source.data == original.data && source.bytes == original.bytes ? Utf8View{} : original,
        resource,
        format,
        animated || format == MediaAttachmentFormat::gif || format == MediaAttachmentFormat::mp4 ||
            (format == MediaAttachmentFormat::webp && (media.flags & 32u) != 0)};
    return true;
}

namespace {
bool embed_media_valid(const MessageRichEmbedMedia& media) noexcept {
    return media.url.bytes != 0 || media.proxy_url.bytes != 0;
}
// PH embeds can carry signed media captured when the message was posted.
// Resolve the stable video identity on activation; never fetch that stale MP4
// merely to paint its poster.
Utf8View renewable_embed_video(const MessageRichStore& store, const MessageRichEmbed& embed) noexcept {
    const auto page = message_rich_store_string(store, embed.url);
    const auto page_video = embed_video_locator(page);
    if (page_video.provider == EmbedProvider::pornhub && page_video.id.bytes)
        return page;
    const auto frame = message_rich_store_string(store, embed.video.url);
    const auto frame_video = embed_video_locator(frame);
    return frame_video.provider == EmbedProvider::pornhub && frame_video.id.bytes ? frame : Utf8View{};
}
Utf8View live_channel_video(const MessageRichStore& store, const MessageRichEmbed& embed) noexcept {
    const auto page = message_rich_store_string(store, embed.url);
    const auto locator = embed_video_locator(page);
    return locator.provider == EmbedProvider::twitch_live && locator.id.bytes ? page : Utf8View{};
}
bool embed_playback_poster(const MessageRichStore& store, const MessageRichEmbed& embed,
                         MessageRichEmbedMediaSelection* out) noexcept {
    if (select_embed_media_candidate(store, embed.image, MediaEmbedResource::image, false, out) ||
        select_embed_media_candidate(store, embed.thumbnail, MediaEmbedResource::thumbnail, false, out)) {
        if ((embed.video.state & 3u) == 3u && embed.video.width && embed.video.height)
            out->playback_extent = &embed.video;
        return true;
    }
    // Text-only playable cards still reserve a native play surface. No poster
    // URL is invented or fetched; the existing media extent supplies its default.
    *out = {&embed.video, {}, {}, MediaEmbedResource::video, MediaAttachmentFormat::mp4, false};
    if ((embed.video.state & 3u) == 3u && embed.video.width && embed.video.height)
        out->playback_extent = &embed.video;
    return true;
}
// ARTICLE and IMAGE thumbnails are normalized into the large image.
bool embed_thumbnail_is_image(const MessageRichEmbed& embed) noexcept {
    return (embed.type == GatewayMessageEmbedType::article || embed.type == GatewayMessageEmbedType::image) &&
           !embed_media_valid(embed.image) && embed_media_valid(embed.thumbnail);
}
Utf8View soundcloud_audio_url(const MessageRichStore& store, const MessageRichEmbed& embed) noexcept {
    const auto page = message_rich_store_string(store, embed.url);
    if (soundcloud_track_url(page) || soundcloud_collection_url(page)) return page;
    const auto player = message_rich_store_string(store, embed.video.url);
    return soundcloud_track_url(player) || soundcloud_collection_url(player) ? player : Utf8View{};
}
Utf8View apple_collection_url(const MessageRichStore& store,const MessageRichEmbed& embed) noexcept {
    const auto page=message_rich_store_string(store,embed.url), player=message_rich_store_string(store,embed.video.url);
    return apple_music_collection_locator(page).id.bytes ? page : apple_music_collection_locator(player).id.bytes ? player : Utf8View{};
}
Utf8View youtube_queue_url(const MessageRichStore& store, const MessageRichEmbed& embed) noexcept {
    const auto page = message_rich_store_string(store, embed.url), player = message_rich_store_string(store, embed.video.url);
    return youtube_collection_locator(page).list.bytes ? page : youtube_collection_locator(player).list.bytes ? player : Utf8View{};
}
} // namespace

bool message_rich_embed_music_preview(const MessageRichStore& store, const MessageRichEmbed& embed) noexcept {
    return embed.thumbnail.width && embed.thumbnail.height && embed_media_valid(embed.thumbnail) &&
           (spotify_audio_locator(message_rich_store_string(store, embed.url)).bytes != 0 ||
            amazon_music_track_locator(message_rich_store_string(store, embed.url)).id.bytes != 0 ||
            apple_music_track_locator(message_rich_store_string(store, embed.url)).id.bytes != 0 ||
            apple_music_collection_locator(message_rich_store_string(store, embed.url)).id.bytes != 0);
}
Utf8View message_rich_embed_provider_label(const MessageRichStore& store, const MessageRichEmbed& embed) noexcept {
    if (apple_collection_url(store,embed).bytes) return utf8_literal("Apple Music · Preview");
    return message_rich_embed_music_preview(store, embed)
               ? (embed_provider(message_rich_store_string(store, embed.url)) == EmbedProvider::apple_music
                      ? utf8_literal("Apple Music · Preview")
                      : embed_provider(message_rich_store_string(store, embed.url)) == EmbedProvider::amazon_music
                          ? utf8_literal("Amazon Music · Preview") : utf8_literal("Spotify · Preview"))
                                                         : message_rich_store_string(store, embed.provider_name);
}
bool message_rich_embed_media_select(const MessageRichStore& store, const MessageRichEmbed& embed,
                                     MessageRichEmbedMediaSelection* selection) noexcept {
    if (!selection) {
        return false;
    }
    reset_value<MessageRichEmbedMediaSelection>(selection);
    if (!g_display.embed_media)
        return false;
    // Media order: gifv plays its video; any other embed with a
    // video shows the video, whose thumbnail is its poster (and the fallback
    // when the video itself cannot be presented); then the image, including a
    // normalized article/image thumbnail.
    if (embed.type == GatewayMessageEmbedType::gifv &&
        select_embed_media_candidate(store, embed.video, MediaEmbedResource::video, true, selection)) {
        return true;
    }
    if (live_channel_video(store, embed).bytes || soundcloud_audio_url(store, embed).bytes ||
        apple_collection_url(store,embed).bytes || youtube_queue_url(store,embed).bytes)
        return embed_playback_poster(store, embed, selection);
    if (message_rich_embed_music_preview(store, embed) || renewable_embed_video(store, embed).bytes)
        return select_embed_media_candidate(store, embed.thumbnail, MediaEmbedResource::thumbnail, false, selection);
    if (embed_media_valid(embed.video) &&
        (select_embed_media_candidate(store, embed.video, MediaEmbedResource::video, false, selection) ||
         select_embed_media_candidate(store, embed.thumbnail, MediaEmbedResource::thumbnail, false,
                                      selection))) {
        return true;
    }
    return select_embed_media_candidate(store, embed.image, MediaEmbedResource::image, false, selection) ||
           ((embed_thumbnail_is_image(embed) || message_rich_embed_media_only(embed)) &&
            select_embed_media_candidate(store, embed.thumbnail, MediaEmbedResource::thumbnail, false,
                                         selection));
}

bool message_rich_embed_playback_select(const MessageRichStore& store, const MessageRichEmbed& embed,
                                       MessageRichEmbedMediaSelection* out) noexcept {
    if (!out) return false;
    reset_value(out);
    if (!g_display.embed_media || embed.type == GatewayMessageEmbedType::gifv) return false;
    if (const auto queue_url = youtube_queue_url(store,embed); queue_url.bytes) {
        embed_playback_poster(store, embed, out);
        *out = {out->media, queue_url, {}, MediaEmbedResource::video, MediaAttachmentFormat::mp4, true, true};
        return true;
    }
    if (const auto album_url=apple_collection_url(store,embed); album_url.bytes) {
        embed_playback_poster(store,embed,out);
        *out={out->media,album_url,{},MediaEmbedResource::video,MediaAttachmentFormat::m4a,true,true}; return true;
    }
    const auto audio_url = soundcloud_audio_url(store, embed);
    if (audio_url.bytes) {
        embed_playback_poster(store, embed, out);
        *out = {out->media, audio_url, {}, MediaEmbedResource::video, MediaAttachmentFormat::m4a, false, true};
        return true;
    }
    if (const auto url = live_channel_video(store, embed); url.bytes) {
        embed_playback_poster(store, embed, out);
        *out = {out->media, url, {}, MediaEmbedResource::video, MediaAttachmentFormat::mp4, true, true};
        return true;
    }
    if (const auto url = renewable_embed_video(store, embed); url.bytes &&
        embed.thumbnail.width && embed.thumbnail.height && embed_media_valid(embed.thumbnail)) {
        *out = {&embed.thumbnail, url, {}, MediaEmbedResource::video, MediaAttachmentFormat::mp4, true, true};
        return true;
    }
    if (message_rich_embed_media_select(store, embed, out) && out->format == MediaAttachmentFormat::mp4)
        return true;
    if (message_rich_embed_music_preview(store, embed)) {
        const auto url = message_rich_store_string(store, embed.url);
        const auto apple = apple_music_track_locator(url);
        *out = {&embed.thumbnail, message_rich_store_string(store, embed.url), {}, MediaEmbedResource::video,
                apple.id.bytes ? (apple.video ? MediaAttachmentFormat::mp4 : MediaAttachmentFormat::m4a)
                               : MediaAttachmentFormat::mp3, apple.video, true};
        return true;
    }
    const auto url = message_rich_store_string(store, embed.video.url);
    const auto locator = embed_video_locator(url);
    if (embed.video.proxy_url.bytes || !locator.id.bytes ||
        !embed.thumbnail.width || !embed.thumbnail.height || !embed_media_valid(embed.thumbnail)) {
        reset_value(out);
        return false;
    }
    *out = {&embed.video, url, {}, MediaEmbedResource::video, MediaAttachmentFormat::mp4, true, true};
    out->start_seconds = locator.start_seconds;
    // Vimeo oEmbed may retain the iframe's canonical video URL but omit the shared
    // link's fragment. Only the same exact provider/video may supply its time.
    if (locator.provider == EmbedProvider::vimeo) {
        const auto page = embed_video_locator(message_rich_store_string(store, embed.url));
        if (page.provider == locator.provider && page.id.bytes && utf8_equal(page.id, locator.id) && page.start_seconds)
            out->start_seconds = page.start_seconds;
    }
    return true;
}

MessageRichEmbedPlan message_rich_embed_plan(const MessageRichStore& store, const MessageRichEmbed& embed) noexcept {
    const GatewayMessageEmbedType type = embed.type;
    const bool components = type == GatewayMessageEmbedType::components;
    const bool rich = type == GatewayMessageEmbedType::rich || type == GatewayMessageEmbedType::unknown;
    MessageRichEmbedPlan plan{};
    plan.media_only = message_rich_embed_media_only(embed);
    // A components embed's text displays use the message markdown rules at
    // message size.
    plan.markdown = rich || components;
    plan.components = components;
    plan.description = type != GatewayMessageEmbedType::image && type != GatewayMessageEmbedType::video &&
                       type != GatewayMessageEmbedType::gifv;
    // A poll result shows its own card text, never its raw fields.
    plan.fields = rich;
    const bool video = embed_media_valid(embed.video) || message_rich_embed_music_preview(store, embed) ||
                       live_channel_video(store, embed).bytes || youtube_queue_url(store,embed).bytes;
    const bool large = video || embed_media_valid(embed.image) || embed_thumbnail_is_image(embed);
    plan.corner_thumbnail = !plan.media_only && embed_media_valid(embed.thumbnail) &&
                            !embed_thumbnail_is_image(embed) && (rich || components ? !video : !large);
    return plan;
}

Utf8View message_rich_embed_author_icon(const MessageRichStore& store, const MessageRichEmbed& embed) noexcept {
    return embed.author_name.bytes && !message_rich_embed_media_only(embed)
               ? message_rich_store_string(store, embed.author_icon)
               : Utf8View{};
}

Utf8View message_rich_embed_footer_icon(const MessageRichStore& store, const MessageRichEmbed& embed) noexcept {
    return embed.footer_text.bytes && !message_rich_embed_media_only(embed)
               ? message_rich_store_string(store, embed.footer_icon)
               : Utf8View{};
}

// The small icon beside an embed's author or footer line.
static bool embed_icon_rect(bool present, const float* tops, u32 index, float extent, UiRect content,
                            UiRect* output) noexcept {
    if (!output)
        return false;
    reset_value(output);
    if (index >= kGatewayMessageEmbedMaximum || !present || !(tops[index] > 0.0f))
        return false;
    const float left = content.left + kMessageEmbedContentInset;
    const float top = content.top + tops[index];
    *output = {left, top, left + extent, top + extent};
    return true;
}

bool message_rich_embed_author_icon_rect(const MessageRichStore& store, const MessageRichEmbed& embed,
                                         const MessageRichItemLayout& layout, u32 index, UiRect content,
                                         UiRect* output) noexcept {
    return embed_icon_rect(message_rich_embed_author_icon(store, embed).bytes != 0,
                           layout.embed_author_tops, index, kMessageEmbedAuthorIcon, content, output);
}

bool message_rich_embed_footer_icon_rect(const MessageRichStore& store, const MessageRichEmbed& embed,
                                         const MessageRichItemLayout& layout, u32 index, UiRect content,
                                         UiRect* output) noexcept {
    return embed_icon_rect(message_rich_embed_footer_icon(store, embed).bytes != 0,
                           layout.embed_footer_tops, index, kMessageEmbedFooterIcon, content, output);
}

MediaAttachmentFormat message_rich_embed_icon_format(Utf8View url) noexcept {
    const MediaAttachmentFormat format = media_format_from_url(url);
    return format == MediaAttachmentFormat::jpeg || format == MediaAttachmentFormat::webp ||
                   format == MediaAttachmentFormat::gif
               ? format
               : MediaAttachmentFormat::png;
}

UiRect message_rich_embed_thumbnail_rect(UiRect card, float width, float height, bool components) noexcept {
    const float top = card.top + message_rich_embed_top_inset(components) + kMessageEmbedBlockGap;
    return {card.right - kMessageEmbedEndInset - width, top, card.right - kMessageEmbedEndInset, top + height};
}

bool message_rich_embed_resource_select(const MessageRichStore& store, const MessageRichEmbed& embed,
                                        MediaEmbedResource resource,
                                        MessageRichEmbedMediaSelection* out) noexcept {
    if (!g_display.embed_media)
        return false;
    switch (resource) {
    case MediaEmbedResource::image:
        return select_embed_media_candidate(store, embed.image, resource, false, out);
    case MediaEmbedResource::thumbnail:
        return select_embed_media_candidate(store, embed.thumbnail, resource, false, out);
    case MediaEmbedResource::video: {
        MessageRichEmbedMediaSelection playback{};
        if (message_rich_embed_playback_select(store, embed, &playback) && playback.provider) {
            if (!out) return false;
            *out = playback;
            return true;
        }
        return select_embed_media_candidate(store, embed.video, resource,
                                            embed.type == GatewayMessageEmbedType::gifv, out);
    }
    default:
        return false;
    }
}

bool message_rich_embed_thumbnail_extent(const MessageRichStore& store, const MessageRichEmbed& embed,
                                         float available_width, float* width, float* height) noexcept {
    MessageRichEmbedMediaSelection selection{};
    return message_rich_embed_plan(store, embed).corner_thumbnail &&
           message_rich_embed_resource_select(store, embed, MediaEmbedResource::thumbnail, &selection) &&
           bounded_media_extent(selection.media->width ? selection.media->width : 80,
                                selection.media->height ? selection.media->height : 80, available_width,
                                kMessageEmbedThumbnailExtent, kMessageEmbedThumbnailExtent, width, height);
}

bool message_rich_embed_media_only(const MessageRichEmbed& embed) noexcept {
    const auto valid = [](const MessageRichEmbedMedia& media) noexcept {
        return media.url.bytes != 0 || media.proxy_url.bytes != 0;
    };
    // IMAGE/ARTICLE thumbnails are normalized to images before the inline
    // predicate. Rich cards remain cards, even when their only content is an
    // image. This is metadata policy, not a host list.
    const bool media =
        valid(embed.image) || valid(embed.video) ||
        ((embed.type == GatewayMessageEmbedType::image || embed.type == GatewayMessageEmbedType::article) &&
         valid(embed.thumbnail));
    return media && (embed.type == GatewayMessageEmbedType::gifv ||
                     (embed.type != GatewayMessageEmbedType::rich &&
                      embed.type != GatewayMessageEmbedType::components && !embed.has_author &&
                      !embed.has_title && embed.author_name.bytes == 0 && embed.title.bytes == 0));
}

u32 message_rich_embed_merge_host(const MessageRichStore& store, const MessageRichRecord& rich,
                                  u32 index) noexcept {
    if (index >= rich.embed_count || !rich.embeds[index].url.bytes)
        return index;
    const Utf8View url = message_rich_store_string(store, rich.embeds[index].url);
    for (u32 i = 0; i < index; ++i)
        if (rich.embeds[i].url.bytes == url.bytes &&
            view_equal(message_rich_store_string(store, rich.embeds[i].url), url))
            return i;
    return index;
}

namespace {
// The normalized embed image: image, else an article/image thumbnail.
const MessageRichEmbedMedia* gallery_image(const MessageRichEmbed& embed) noexcept {
    if (embed_media_valid(embed.image))
        return &embed.image;
    return (embed.type == GatewayMessageEmbedType::article || embed.type == GatewayMessageEmbedType::image) &&
                   embed_media_valid(embed.thumbnail)
               ? &embed.thumbnail
               : nullptr;
}
} // namespace

bool message_rich_embed_gallery(const MessageRichStore& store, const MessageRichRecord& rich, u32 host,
                                MessageRichEmbedGallery* out) noexcept {
    if (!out)
        return false;
    reset_value(out);
    if (host >= rich.embed_count || !g_display.embed_media || !rich.embeds[host].url.bytes ||
        message_rich_embed_merge_host(store, rich, host) != host)
        return false;
    const MessageRichEmbed& first = rich.embeds[host];
    // Later embeds with the same url add their images to one gallery.
    bool merged = false;
    for (u32 i = host + 1; i < rich.embed_count; ++i)
        if (gallery_image(rich.embeds[i]) && message_rich_embed_merge_host(store, rich, i) == host) {
            if (!merged && gallery_image(first))
                out->members[out->count++] = static_cast<u8>(host);
            merged = true;
            out->members[out->count++] = static_cast<u8>(i);
        }
    // A gifv or any video plays instead of the images.
    if (!merged || first.type == GatewayMessageEmbedType::gifv || embed_media_valid(first.video)) {
        reset_value(out);
        return false;
    }
    return true;
}

bool message_rich_embed_gallery_select(const MessageRichStore& store, const MessageRichEmbed& embed,
                                       MessageRichEmbedMediaSelection* selection) noexcept {
    const MessageRichEmbedMedia* media = gallery_image(embed);
    return media && g_display.embed_media &&
           select_embed_media_candidate(store, *media,
                                        media == &embed.image ? MediaEmbedResource::image
                                                              : MediaEmbedResource::thumbnail,
                                        false, selection);
}

float message_rich_embed_gallery_width(const MessageRichStore&, const MessageRichEmbed& host,
                                       float available_width) noexcept {
    constexpr float insets = kMessageEmbedContentInset + kMessageEmbedEndInset;
    float target = kMessageEmbedMaximumWidth - insets;
    if (message_rich_embed_media_only(host)) {
        target = 400.0f;
    } else if (const MessageRichEmbedMedia* image = gallery_image(host)) {
        float width = 0.0f, height = 0.0f;
        if (bounded_media_extent(image->width ? image->width : 400, image->height ? image->height : 225, 400.0f,
                                 400.0f, 300.0f, &width, &height) &&
            width >= 300.0f)
            target = width;
    }
    return maximum(0.0f, minimum(target, available_width));
}

UiRect message_rich_embed_gallery_tile(UiRect gallery, u32 count, u32 ordinal) noexcept {
    if (!count || ordinal >= minimum(count, kMessageEmbedGalleryDrawn))
        return {};
    const float column = maximum(0.0f, (ui_rect_width(gallery) - kMessageEmbedGalleryGap) * 0.5f);
    const float half = maximum(0.0f, (ui_rect_height(gallery) - kMessageEmbedGalleryGap) * 0.5f);
    // Column 1: images 1 and 3 (when four); column 2: image 2, then 3 or 4.
    const bool right = ordinal == 1 || (ordinal == 2 && count == 3) || ordinal == 3;
    const u32 in_column = right ? (count >= 3 ? 2u : 1u) : (count >= 4 ? 2u : 1u);
    const bool lower = ordinal == 3 || (ordinal == 2 && count >= 3);
    const float left = right ? gallery.left + column + kMessageEmbedGalleryGap : gallery.left;
    if (in_column == 1)
        return {left, gallery.top, left + column, gallery.bottom};
    const float top = lower ? gallery.top + half + kMessageEmbedGalleryGap : gallery.top;
    return {left, top, left + column, top + half};
}

bool message_rich_embed_gallery_place(const MessageRichStore& store, const MessageRichRecord& rich, u32 index,
                                      MessageRichEmbedGalleryPlace* out) noexcept {
    if (!out)
        return false;
    reset_value(out);
    const u32 host = message_rich_embed_merge_host(store, rich, index);
    MessageRichEmbedGallery gallery{};
    if (!message_rich_embed_gallery(store, rich, host, &gallery))
        return false;
    for (u32 i = 0; i < gallery.count && i < kMessageEmbedGalleryDrawn; ++i)
        if (gallery.members[i] == index) {
            *out = {host, i, gallery.count};
            return true;
        }
    return false;
}

UiRect message_rich_embed_media_rect(const MessageRichItemLayout& layout, u32 index, UiRect content,
                                     bool media_only) noexcept {
    if (index >= kGatewayMessageEmbedMaximum || !(layout.embed_media_widths[index] > 0.0f) ||
        !(layout.embed_media_heights[index] > 0.0f))
        return {};
    const float left = content.left + message_rich_embed_media_left(media_only);
    const float top = content.top + layout.embed_media_tops[index];
    return {left, top, left + layout.embed_media_widths[index], top + layout.embed_media_heights[index]};
}

bool message_rich_embed_suppresses_content(const MessageRichStore& store, const MessageRichRecord& rich,
                                           Utf8View content) noexcept {
    if (content.bytes == 0 || rich.embed_count != 1 || !g_display.embeds || !g_display.embed_media) {
        return false;
    }
    for (u32 index = 0; index < rich.embed_count; ++index) {
        const MessageRichEmbed& embed = rich.embeds[index];
        if ((embed.type == GatewayMessageEmbedType::image || embed.type == GatewayMessageEmbedType::gifv) &&
            message_rich_embed_media_only(embed) &&
            view_equal(content, message_rich_store_string(store, embed.url))) {
            return true;
        }
    }
    return false;
}

bool message_rich_embed_media_extent(const MessageRichEmbedMediaSelection& selection, float available_width,
                                     float* width, float* height) noexcept {
    if (!selection.media || !width || !height || available_width <= 0.0f ||
        selection.format == MediaAttachmentFormat::unknown) {
        return false;
    }
    if (const auto* video = selection.playback_extent)
        return bounded_media_extent(video->width, video->height, available_width,
                                    kMessageEmbedMaximumWidth, 300.0f, width, height);
    // Embed media defaults to 400x225. Attachment mosaics have a separate
    // policy; an inline URL image must not inherit their larger bounds or the
    // enclosing rich card's width.
    return bounded_media_extent(selection.media->width ? selection.media->width : 400,
                                selection.media->height ? selection.media->height : 225, available_width,
                                400.0f, 300.0f, width, height);
}

float message_rich_embed_visual_height(const MessageRichStore& store, const MessageRichRecord& rich,
                                       const MessageRichEmbed& embed) noexcept {
    // Text blocks only (the media block is added by the caller); the text
    // pass replaces this estimate with measured lines.
    const MessageRichEmbedPlan plan = message_rich_embed_plan(store, embed);
    const float top = message_rich_embed_top_inset(plan.components);
    float height = top + kMessageEmbedBottomInset;
    const auto block = [&](float extent) noexcept { height += kMessageEmbedBlockGap + extent; };
    if (message_rich_embed_provider_label(store, embed).bytes)
        block(18.0f);
    if (embed.author_name.bytes)
        block(message_rich_embed_author_icon(store, embed).bytes ? kMessageEmbedAuthorIcon : 22.0f);
    if (embed.title.bytes)
        block(22.0f);
    if (embed.description.bytes && plan.description)
        block(message_rich_store_string(store, embed.description).bytes > 40 ? 44.0f : 22.0f);
    if (plan.fields && embed.field_count)
        height += static_cast<float>(embed_field_row_count(rich, embed)) * (kMessageEmbedBlockGap + 40.0f);
    if (embed.type == GatewayMessageEmbedType::poll_result)
        block(36.0f);
    if (embed.footer_text.bytes || embed.timestamp_ms)
        block(kMessageEmbedFooterIcon);
    float thumbnail_width = 0.0f, thumbnail_height = 0.0f;
    if (message_rich_embed_thumbnail_extent(store, embed, kMessageEmbedThumbnailExtent, &thumbnail_width,
                                            &thumbnail_height))
        height = maximum(height, top + kMessageEmbedBlockGap + thumbnail_height + kMessageEmbedBottomInset);
    return height;
}

float message_rich_component_visual_height(const MessageRichComponent& component) noexcept {
    return message_component_leaf_height(component);
}

float message_rich_reaction_button_width(const MessageRichReaction& reaction) noexcept {
    u32 digits = 1;
    for (u32 count = reaction.count; count >= 10; count /= 10) {
        ++digits;
    }
    // Reaction labels use the 14-DIP UI face; its tabular decimal advance is
    // estimated at 8 DIP and emoji occupy a 16-DIP media cell. This keeps layout
    // deterministic before the text pass measures the final glyphs.
    constexpr float digit_advance = 8.0f;
    constexpr float unicode_gap = 4.0f;
    const float emoji_and_gap =
        kDefaultMessageReactionLayoutMetrics.emoji_extent +
        (reaction.emoji_id != 0 ? kDefaultMessageReactionLayoutMetrics.emoji_gap : unicode_gap);
    const float unicode_inset =
        reaction.emoji_id == 0 ? kDefaultMessageReactionLayoutMetrics.unicode_text_inset : 0.0f;
    return kDefaultMessageReactionLayoutMetrics.content_left_padding + unicode_inset + emoji_and_gap +
           static_cast<float>(digits) * digit_advance +
           kDefaultMessageReactionLayoutMetrics.content_right_padding;
}

float message_rich_reaction_layout_height(const MessageRichRecord& rich, float available_width,
                                          ConstSlice<float> intrinsic_widths) noexcept {
    if (rich.reaction_count == 0 || available_width <= 0.0f || !g_display.reactions) {
        return 0.0f;
    }
    u32 rows = 1;
    float row_width = 0.0f;
    for (u32 index = 0; index < rich.reaction_count; ++index) {
        const float measured = index < intrinsic_widths.count ? intrinsic_widths[index] : 0.0f;
        const float width =
            measured > 0.0f ? measured : message_rich_reaction_button_width(rich.reactions[index]);
        const float required =
            row_width == 0.0f ? width : row_width + kDefaultMessageReactionLayoutMetrics.gap + width;
        if (row_width != 0.0f && required > available_width) {
            ++rows;
            row_width = width;
        } else {
            row_width = required;
        }
    }
    return static_cast<float>(rows) * kDefaultMessageReactionLayoutMetrics.button_height +
           static_cast<float>(rows - 1) * kDefaultMessageReactionLayoutMetrics.gap;
}

MessageRichItemLayout message_rich_estimate_item_layout(const MessageRichStore* store,
                                                        const MessageRichRecord* rich,
                                                        float available_width,
                                                        const UiResolvedThemeStyles* styles) noexcept {
    MessageRichItemLayout result{};
    if (!store || !rich || available_width <= 0.0f) {
        return result;
    }
    for (u32 index = 0; index < rich->attachment_count; ++index) {
        float height = 42.0f;
        float preview_width = 0.0f;
        float preview_height = 0.0f;
        if (message_rich_attachment_audio_supported(*store, rich->attachments[index])) {
            const float audio_height =
                ui_audio_card_height(ui_theme_audio_style(styles), available_width, rich->voice_message);
            if (audio_height > 0)
                height = audio_height;
        } else if (message_rich_attachment_preview_extent(*store, rich->attachments[index], available_width,
                                                          &preview_width, &preview_height)) {
            height = preview_height + 6.0f;
        }
        result.heights[kMessageRichAttachmentItemOffset + index] = height;
    }
    for (u32 index = 0; g_display.embeds && index < rich->embed_count; ++index) {
        // A merged embed's images draw in the first embed's gallery.
        if (message_rich_embed_merge_host(*store, *rich, index) != index)
            continue;
        const bool media_only = message_rich_embed_media_only(rich->embeds[index]);
        float height =
            media_only ? 0.0f : message_rich_embed_visual_height(*store, *rich, rich->embeds[index]);
        MessageRichEmbedMediaSelection media{};
        MessageRichEmbedGallery gallery{};
        float media_width = 0.0f;
        float media_height = 0.0f;
        const float media_available =
            maximum(0.0f, minimum(available_width, kMessageEmbedMaximumWidth) -
                              (media_only ? 0.0f : kMessageEmbedContentInset + kMessageEmbedEndInset));
        if (message_rich_embed_gallery(*store, *rich, index, &gallery)) {
            media_width = message_rich_embed_gallery_width(*store, rich->embeds[index], media_available);
            media_height = kMessageEmbedGalleryHeight;
        }
        if ((media_width > 0.0f && media_height > 0.0f) ||
            (!gallery.count && message_rich_embed_media_select(*store, rich->embeds[index], &media) &&
             message_rich_embed_media_extent(media, media_available, &media_width, &media_height))) {
            const bool footer = rich->embeds[index].footer_text.bytes || rich->embeds[index].timestamp_ms;
            const float footer_height = footer ? kMessageEmbedBlockGap + kMessageEmbedFooterIcon : 0.0f;
            result.embed_media_tops[index] =
                media_only ? 0.0f
                           : maximum(kMessageEmbedMediaGap, height - kMessageEmbedBottomInset - footer_height +
                                                                kMessageEmbedMediaGap);
            result.embed_media_widths[index] = media_width;
            result.embed_media_heights[index] = media_height;
            height += media_height + (media_only ? 0.0f : kMessageEmbedMediaGap);
        }
        result.heights[kMessageRichEmbedItemOffset + index] = height + (media_only ? 0.0f : kMessageEmbedItemGap);
    }
    for (u32 index = 0; index < rich->sticker_count; ++index) {
        result.heights[kMessageRichStickerItemOffset + index] =
            kDefaultMessageStickerLayoutMetrics.minimum_height();
    }
    for (u32 index = 0; index < rich->component_count; ++index) {
        result.heights[kMessageRichComponentItemOffset + index] =
            message_rich_component_visual_height(rich->components[index]);
        result.component_widths[index] = message_component_leaf_width(
            rich->components[index], minimum(available_width, kMessageComponentMaximumWidth) -
                                         2 * kMessageComponentPadding);
    }
    // Button widths are placeholders until shaped: rows wrap in the text pass.
    message_component_measure_tree(*rich, &result);
    if (rich->truncations != 0) {
        result.heights[kMessageRichNoticeItemOffset] = 22.0f;
    }
    return result;
}

MessageRichLayout message_rich_aggregate_layout(const MessageRichRecord& rich,
                                                const MessageRichItemLayout& items, float available_width,
                                                ConstSlice<float> reaction_widths) noexcept {
    MessageRichLayout result{};
    for (u32 index = 0; index < rich.attachment_count; ++index) {
        result.attachments += items.heights[kMessageRichAttachmentItemOffset + index];
    }
    for (u32 index = 0; index < rich.embed_count; ++index) {
        result.embeds += items.heights[kMessageRichEmbedItemOffset + index];
    }
    for (u32 index = 0; index < rich.sticker_count; ++index) {
        result.stickers += items.heights[kMessageRichStickerItemOffset + index];
    }
    u32 component_roots = 0;
    for (u32 index = 0; index < rich.component_count; ++index) {
        if (rich.components[index].parent == 0xFFFFu) {
            result.components += items.heights[kMessageRichComponentItemOffset + index];
            ++component_roots;
        }
    }
    // Root components stack with the component gap.
    if (component_roots > 1)
        result.components += static_cast<float>(component_roots - 1) * kMessageComponentGap;
    result.reactions = message_rich_reaction_layout_height(rich, available_width, reaction_widths);
    // Truncation notices and message annotations (ephemeral footer, failed
    // interaction) share the notice item; the estimate seeds only truncation.
    result.notice = items.heights[kMessageRichNoticeItemOffset];
    result.thread = rich.thread.id && !rich.forwarded_snapshot_child
                        ? kMessageThreadCardGapDips + kMessageThreadCardHeightDips
                        : 0.0f;
    // Invite cards come from the content, not the rich record; their item
    // heights are set by the text pass (zero for unused slots).
    u32 invites = 0;
    for (u32 index = 0; index < kMessageInviteCardMaximum; ++index) {
        const float height = items.heights[kMessageRichInviteItemOffset + index];
        result.invites += height;
        invites += height > 0.0f ? 1u : 0u;
    }
    if (invites > 1)
        result.invites += static_cast<float>(invites - 1) * kMessageInviteCardGap;
    return result;
}

MessageRichLayout message_rich_measure_layout(const MessageRichStore* store, const MessageRichRecord* rich,
                                              float available_width,
                                              const UiResolvedThemeStyles* styles) noexcept {
    if (!store || !rich || available_width <= 0.0f) {
        return MessageRichLayout{};
    }
    return message_rich_aggregate_layout(
        *rich, message_rich_estimate_item_layout(store, rich, available_width, styles), available_width);
}

} // namespace maicord
