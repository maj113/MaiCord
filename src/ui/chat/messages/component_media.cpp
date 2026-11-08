#include "maicord/ui/chat/messages/component_media.hpp"

#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/chat/message_rich_store.hpp"
#include "maicord/protocol/guild/community.hpp"
#include "maicord/media/core/asset.hpp"

namespace maicord {
namespace {

bool has_extension(const Utf8View& path, const char* extension) noexcept {
    const u32 bytes = c_string_length(extension);
    if (path.bytes < bytes)
        return false;
    for (u32 i = 0; i < bytes; ++i)
        if (ascii_lower(path.data[path.bytes - bytes + i]) != extension[i])
            return false;
    return true;
}

} // namespace

MediaAttachmentFormat message_component_media_format(Utf8View url) noexcept {
    // The path ends before any query or fragment.
    u32 end = 0;
    while (end < url.bytes && url.data[end] != '?' && url.data[end] != '#')
        ++end;
    const Utf8View path{url.data, end};
    if (has_extension(path, ".png"))
        return MediaAttachmentFormat::png;
    if (has_extension(path, ".jpg") || has_extension(path, ".jpeg"))
        return MediaAttachmentFormat::jpeg;
    if (has_extension(path, ".webp"))
        return MediaAttachmentFormat::webp;
    if (has_extension(path, ".gif"))
        return MediaAttachmentFormat::gif;
    return MediaAttachmentFormat::unknown;
}

MediaAssetKey message_component_media_key(u64 url_hash, MediaAttachmentFormat format) noexcept {
    MediaAssetKey key = media_asset_embed(url_hash, 1, 0, MediaEmbedResource::image, format, false,
                                          MediaResolutionTier::px512);
    key.kind = MediaAssetKind::profile_image;
    return key;
}

bool message_component_media_resolve(const MessageRichStore& store, const MessageRichComponent& component,
                                     MessageComponentMedia* output) noexcept {
    if (!output || (component.type != gateway_component_thumbnail &&
                    component.type != gateway_component_gallery_item))
        return false;
    const Utf8View url = message_rich_store_string(store, component.url);
    static constexpr char https[] = "https://";
    if (url.bytes <= sizeof(https) - 1u || !bytes_equal(url.data, https, sizeof(https) - 1u))
        return false;
    MediaAttachmentFormat format = message_component_media_format(url);
    // Media proxy URLs without an extension still serve images.
    if (format == MediaAttachmentFormat::unknown)
        format = MediaAttachmentFormat::png;
    *output = {message_component_media_key(hash64_bytes(kHash64Seed, url.data, url.bytes), format), url,
               static_cast<u8>(format)};
    return true;
}

namespace {
// Callers pass a nonzero id.
u32 cdn_url(const Utf8View& prefix, u64 id, const Utf8View& hash, const Utf8View& suffix, const Slice<char>& output) noexcept {
    char digits[20];
    const Utf8View parts[] = {prefix, {digits, decimal_write(digits, id)}, utf8_literal("/"), hash, suffix};
    u32 bytes = 0;
    for (const Utf8View& part : parts) {
        if (part.bytes > output.count - bytes)
            return 0;
        copy_bytes(output.data + bytes, part.data, part.bytes);
        bytes += part.bytes;
    }
    return bytes;
}
} // namespace

u32 message_event_cover_url(const ScheduledEvent& event, const Slice<char>& output) noexcept {
    if (!event.id || !event.image_bytes || !output.data)
        return 0;
    for (u32 i = 0; i < event.image_bytes; ++i) {
        const char c = event.image[i];
        if (!(ascii_is_alnum(c) || c == '_'))
            return 0;
    }
    return cdn_url(utf8_literal("https://cdn.discordapp.com/guild-events/"), event.id, {event.image, event.image_bytes},
                   utf8_literal(".png?size=256"), output);
}

u32 message_invite_icon_url(const GuildInvite& invite, const Slice<char>& output) noexcept {
    if (!invite.guild_id || !invite.icon_bytes || !output.data)
        return 0;
    return cdn_url(utf8_literal("https://cdn.discordapp.com/icons/"), invite.guild_id,
                   {invite.icon, invite.icon_bytes}, utf8_literal(".png?size=128"), output);
}

} // namespace maicord
