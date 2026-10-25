#include "maicord/ui/message_component_media.hpp"

#include "maicord/core/ascii.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/protocol/guild_community.hpp"

namespace maicord {
namespace {

bool has_extension(Utf8View path, const char* extension) noexcept {
    u32 bytes = 0;
    while (extension[bytes])
        ++bytes;
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
u32 cdn_url(Utf8View prefix, u64 id, Utf8View hash, Utf8View suffix, Slice<char> output) noexcept {
    char digits[24];
    u32 id_bytes = 0;
    for (u64 value = id; value; value /= 10u)
        digits[id_bytes++] = static_cast<char>('0' + value % 10u);
    const Utf8View parts[] = {prefix, {}, utf8_literal("/"), hash, suffix};
    u32 bytes = 0;
    for (u32 p = 0; p < 5; ++p) {
        if (p == 1) {
            if (id_bytes > output.count - bytes)
                return 0;
            for (u32 i = 0; i < id_bytes; ++i)
                output.data[bytes++] = digits[id_bytes - 1u - i];
            continue;
        }
        if (parts[p].bytes > output.count - bytes)
            return 0;
        copy_bytes(output.data + bytes, parts[p].data, parts[p].bytes);
        bytes += parts[p].bytes;
    }
    return bytes;
}
} // namespace

u32 message_event_cover_url(const ScheduledEvent& event, Slice<char> output) noexcept {
    if (!event.id || !event.image_bytes || !output.data)
        return 0;
    for (u32 i = 0; i < event.image_bytes; ++i) {
        const char c = event.image[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'))
            return 0;
    }
    return cdn_url(utf8_literal("https://cdn.discordapp.com/guild-events/"), event.id, {event.image, event.image_bytes},
                   utf8_literal(".png?size=256"), output);
}

u32 message_invite_icon_url(const GuildInvite& invite, Slice<char> output) noexcept {
    if (!invite.guild_id || !invite.icon_bytes || !output.data)
        return 0;
    return cdn_url(utf8_literal("https://cdn.discordapp.com/icons/"), invite.guild_id,
                   {invite.icon, invite.icon_bytes}, utf8_literal(".png?size=128"), output);
}

} // namespace maicord
