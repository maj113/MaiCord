#include "maicord/ui/chat/game_media.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/ui/guild/banner.hpp"
#include "maicord/model/chat/game_store.hpp"

namespace maicord {
namespace {

u32 request_size(u32 dips, u32 dpi) noexcept {
    const u32 pixels = static_cast<u32>((static_cast<u64>(dips) * (dpi ? dpi : 96u) + 95u) / 96u);
    return ui_media_proxy_size(minimum(pixels, kUiGuildImageMaximumPixels));
}

bool starts_with(Utf8View text, u32 at, const char* literal, u32 bytes) noexcept {
    return at + bytes <= text.bytes && bytes_equal(text.data + at, literal, bytes);
}

// The picture's format from the path's extension (PNG when it has none).
MediaAttachmentFormat url_format(Utf8View url) noexcept {
    u32 end = 0;
    while (end < url.bytes && url.data[end] != '?' && url.data[end] != '#')
        ++end;
    u32 dot = end;
    while (dot && url.data[dot - 1] != '.' && url.data[dot - 1] != '/')
        --dot;
    const Utf8View extension{url.data + dot, end - dot};
    if (!dot || url.data[dot - 1] != '.')
        return MediaAttachmentFormat::png;
    if (ascii_equal_ignore_case(extension, utf8_literal("jpg")) || ascii_equal_ignore_case(extension, utf8_literal("jpeg")))
        return MediaAttachmentFormat::jpeg;
    if (ascii_equal_ignore_case(extension, utf8_literal("webp")))
        return MediaAttachmentFormat::webp;
    if (ascii_equal_ignore_case(extension, utf8_literal("gif")))
        return MediaAttachmentFormat::gif;
    return MediaAttachmentFormat::png;
}

bool finish(TextWriter& w, MediaAttachmentFormat format, GameImageMedia* output) noexcept {
    if (!w.ok || !w.used)
        return false;
    output->url_bytes = w.used;
    output->key = media_asset_embed(hash64_bytes(kHash64Seed, output->url, w.used), 1, 0, MediaEmbedResource::image,
                                    format, false, MediaResolutionTier::px512);
    output->key.kind = MediaAssetKind::profile_image;
    return media_asset_key_valid(output->key);
}

bool hash_media(u64 id, Utf8View hash, u32 size_dips, u32 dpi, bool keep_aspect_ratio,
                GameImageMedia* output) noexcept {
    if (!id || !hash.bytes || hash.bytes > 64)
        return false;
    for (u32 i = 0; i < hash.bytes; ++i)
        if (!ascii_is_alnum(hash.data[i]) && hash.data[i] != '_')
            return false;
    TextWriter w{{output->url, kGameImageUrlBytes}};
    w.add("https://cdn.discordapp.com/app-icons/");
    w.number(id);
    w.add("/");
    w.add(hash);
    w.add(".png?size=");
    w.number(request_size(size_dips, dpi));
    if (keep_aspect_ratio)
        w.add("&keep_aspect_ratio=true");
    return finish(w, MediaAttachmentFormat::png, output);
}

bool image(const GuildAdminGame& game, bool cover, u32 size_dips, u32 dpi, GameImageMedia* output) noexcept {
    if (!output || !size_dips)
        return false;
    reset_value(output);
    const Utf8View value = cover ? Utf8View{game.cover, game.cover_bytes} : Utf8View{game.icon, game.icon_bytes};
    const bool url = cover ? game.cover_url : game.icon_url;
    return url ? game_url_media(value, size_dips, dpi, cover, output)
               : hash_media(game.id, value, size_dips, dpi, cover, output);
}

} // namespace

bool game_icon_media(const GuildAdminGame& game, u32 size_dips, u32 dpi, GameImageMedia* output) noexcept {
    return image(game, false, size_dips, dpi, output);
}

bool game_cover_media(const GuildAdminGame& game, u32 size_dips, u32 dpi, GameImageMedia* output) noexcept {
    return image(game, true, size_dips, dpi, output);
}

bool game_chip_icon_media(const GameStore* games, u64 id, GameImageMedia* output) noexcept {
    const GameRecord* record = games ? game_store_find(*games, id) : nullptr;
    return record && record->state == GameState::known && record->game.icon_bytes &&
           game_icon_media(record->game, 32, 96, output);
}

// The official client sizes a CDN URL that names no size yet (?size=) and a
// media proxy URL (/external/: width and height); other hosts serve as is.
bool game_url_media(Utf8View url, u32 size_dips, u32 dpi, bool keep_aspect_ratio, GameImageMedia* output) noexcept {
    if (!output || !url.data || !starts_with(url, 0, "https://", 8))
        return false;
    reset_value(output);
    u32 host_end = 8;
    while (host_end < url.bytes && url.data[host_end] != '/' && url.data[host_end] != '?')
        ++host_end;
    const Utf8View host{url.data + 8, host_end - 8};
    bool query = false, sized = false;
    for (u32 i = host_end; i < url.bytes; ++i) {
        if (url.data[i] == '?')
            query = true;
        if ((url.data[i] == '?' || url.data[i] == '&') && starts_with(url, i + 1, "size=", 5))
            sized = true;
    }
    const bool cdn = ascii_equal_ignore_case(host, utf8_literal("cdn.discordapp.com")) ||
                     ascii_equal_ignore_case(host, utf8_literal("media.discordapp.net"));
    const bool proxy = host.bytes > 25 && starts_with(url, 8, "images-ext-", 11) &&
                       ascii_equal_ignore_case({host.data + host.bytes - 15, 15}, utf8_literal(".discordapp.net")) &&
                       starts_with(url, host_end, "/external/", 10);
    TextWriter w{{output->url, kGameImageUrlBytes}};
    w.add(url);
    const u32 size = request_size(size_dips, dpi);
    if (cdn && !sized) {
        w.add(query ? utf8_literal("&size=") : utf8_literal("?size="));
        w.number(size);
    } else if (proxy) {
        w.add(query ? utf8_literal("&width=") : utf8_literal("?width="));
        w.number(size);
        w.add("&height=");
        w.number(size);
        if (keep_aspect_ratio)
            w.add("&keep_aspect_ratio=true");
    }
    return finish(w, url_format(url), output);
}

} // namespace maicord
