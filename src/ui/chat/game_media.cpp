#include "maicord/ui/chat/game_media.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/ui/chat/messages/component_media.hpp"
#include "maicord/ui/guild/banner.hpp"
#include "maicord/model/chat/game_store.hpp"

namespace maicord {
namespace {

bool starts_with(Utf8View text, u32 at, const char* literal, u32 bytes) noexcept {
    return at + bytes <= text.bytes && bytes_equal(text.data + at, literal, bytes);
}

// The identity of the URL written: the picture's format from the path's extension (PNG when it has none).
bool finish(const TextWriter& w, GameImageMedia* output) noexcept {
    if (!w.ok || !w.used)
        return false;
    output->url_bytes = w.used;
    const auto format = message_component_media_format(output->url_view());
    output->key = message_component_media_key(hash64_bytes(kHash64Seed, output->url, w.used),
                                              format == MediaAttachmentFormat::unknown ? MediaAttachmentFormat::png
                                                                                       : format);
    return media_asset_key_valid(output->key);
}

bool hash_media(u64 id, Utf8View hash, u32 size, bool keep_aspect_ratio, GameImageMedia* output) noexcept {
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
    w.number(size);
    if (keep_aspect_ratio)
        w.add("&keep_aspect_ratio=true");
    return finish(w, output);
}

// The official client sizes a CDN URL that names no size yet (?size=) and a
// media proxy URL (/external/: width and height); other hosts serve as is.
bool url_media(Utf8View url, u32 size, bool keep_aspect_ratio, GameImageMedia* output) noexcept {
    if (!starts_with(url, 0, "https://", 8))
        return false;
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
    if (cdn && !sized) {
        w.add(query ? "&size=" : "?size=", 6);
        w.number(size);
    } else if (proxy) {
        w.add(query ? "&width=" : "?width=", 7);
        w.number(size);
        w.add("&height=");
        w.number(size);
        if (keep_aspect_ratio)
            w.add("&keep_aspect_ratio=true");
    }
    return finish(w, output);
}

bool image(const GuildAdminGame& game, bool cover, u32 size_dips, u32 dpi, GameImageMedia* output) noexcept {
    if (!output || !size_dips)
        return false;
    reset_value(output);
    const Utf8View value = cover ? Utf8View{game.cover, game.cover_bytes} : Utf8View{game.icon, game.icon_bytes};
    const u32 size = ui_guild_image_request_size(size_dips, dpi);
    return (cover ? game.cover_url : game.icon_url) ? url_media(value, size, cover, output)
                                                    : hash_media(game.id, value, size, cover, output);
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

} // namespace maicord
