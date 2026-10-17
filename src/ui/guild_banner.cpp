#include "maicord/ui/guild_banner.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/jpeg.hpp"
#include "maicord/media/webp.hpp"

namespace maicord {
static_assert(kUiGuildImageMaximumPixels <= kJpegDimensionMaximum &&
                  kUiGuildImageMaximumPixels <= kWebpDimensionMaximum,
              "guild image requests must fit the CDN JPEG and WebP decoders");
namespace {
constexpr u16 kProxySizes[]{16,  20,  22,  24,  28,  32,  40,  44,   48,   56,   60,   64,   80,   96,   100, 128,
                            160, 240, 256, 300, 320, 480, 512, 600, 640, 1024, 1280, 1536, 2048, 3072, 4096};

struct Writer {
    char* data;
    u32 capacity, used;
    bool ok;
    void text(Utf8View value) noexcept {
        if (!ok || value.bytes > capacity - used) {
            ok = false;
            return;
        }
        copy_bytes(data + used, value.data, value.bytes);
        used += value.bytes;
    }
    void number(u64 value) noexcept {
        const u32 n = decimal_digits(value);
        if (!ok || n > capacity - used) {
            ok = false;
            return;
        }
        used += decimal_write(data + used, value);
    }
};
// CDN asset hashes: 32 hexadecimal digits, a_ for animated assets.
bool hash_valid(Utf8View hash) noexcept {
    const u32 begin = ui_guild_banner_animated_hash(hash) ? 2u : 0u;
    if (!hash.data || hash.bytes != begin + 32)
        return false;
    for (u32 i = begin; i < hash.bytes; ++i) {
        const char c = hash.data[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}
u32 device_pixels(u32 dips, u32 dpi) noexcept {
    return static_cast<u32>((static_cast<u64>(dips) * (dpi ? dpi : 96u) + 95u) / 96u);
}
// The proxy size a surface of dips asks for: dips * dpr, capped at what the
// decoders accept (512 is itself a proxy size, so the cap never rounds up).
u32 request_size(u32 dips, u32 dpi) noexcept {
    return ui_media_proxy_size(minimum(device_pixels(dips, dpi), kUiGuildImageMaximumPixels));
}
bool finish(Writer& w, MediaAttachmentFormat format, bool animated, UiGuildImageMedia* output) noexcept {
    if (!w.ok)
        return false;
    output->url_bytes = w.used;
    output->key = media_asset_embed(hash64_bytes(kHash64Seed, output->url, w.used), 1, 0, MediaEmbedResource::image,
                                    format, animated, MediaResolutionTier::px512);
    output->key.kind = MediaAssetKind::profile_image;
    return media_asset_key_valid(output->key);
}
float clamp01(float value) noexcept {
    return value > 0 ? (value < 1 ? value : 1.f) : 0.f;
}
} // namespace

u32 ui_media_proxy_size(u32 pixels) noexcept {
    for (const u16 size : kProxySizes)
        if (pixels <= size)
            return size;
    return 4096;
}

bool ui_guild_banner_animated_hash(Utf8View hash) noexcept {
    return hash.data && hash.bytes > 2 && hash.data[0] == 'a' && hash.data[1] == '_';
}

bool ui_guild_banner_media(u64 guild_id, Utf8View hash, u32 dpi, bool animate, bool gif,
                           UiGuildImageMedia* output) noexcept {
    if (!output || !guild_id || !hash_valid(hash))
        return false;
    *output = {};
    const bool animated = animate && ui_guild_banner_animated_hash(hash);
    Writer w{output->url, kUiGuildImageUrlBytes, 0, true};
    w.text(utf8_literal("https://cdn.discordapp.com/banners/"));
    w.number(guild_id);
    w.text(utf8_literal("/"));
    w.text(hash);
    w.text(animated && gif ? utf8_literal(".gif?size=") : utf8_literal(".webp?size="));
    w.number(request_size(360, dpi));
    if (animated && !gif)
        w.text(utf8_literal("&animated=true"));
    return finish(w, animated && gif ? MediaAttachmentFormat::gif : MediaAttachmentFormat::webp, animated, output);
}

bool ui_guild_splash_media(u64 guild_id, Utf8View hash, bool discovery, u32 size_dips, u32 dpi,
                           UiGuildImageMedia* output) noexcept {
    if (!output || !guild_id || !size_dips || !hash_valid(hash))
        return false;
    *output = {};
    Writer w{output->url, kUiGuildImageUrlBytes, 0, true};
    w.text(discovery ? utf8_literal("https://cdn.discordapp.com/discovery-splashes/")
                     : utf8_literal("https://cdn.discordapp.com/splashes/"));
    w.number(guild_id);
    w.text(utf8_literal("/"));
    w.text(hash);
    w.text(utf8_literal(".jpg?size="));
    w.number(request_size(size_dips, dpi));
    return finish(w, MediaAttachmentFormat::jpeg, false, output);
}

bool ui_guild_card_banner_media(u64 guild_id, Utf8View custom_banner, bool discoverable, u32 dpi,
                                UiGuildImageMedia* output) noexcept {
    if (output)
        *output = {};
    return output && discoverable && custom_banner.bytes &&
           ui_guild_splash_media(guild_id, custom_banner, true, kUiGuildCardBannerRequestDips, dpi, output);
}

UiGuildBannerGeometry ui_guild_banner_geometry(UiRect sidebar, UiRect header, float scroll,
                                               bool header_hovered) noexcept {
    UiGuildBannerGeometry g{};
    if (!ui_rect_has_area(sidebar) || !ui_rect_has_area(header) || !(scroll == scroll))
        return g;
    scroll = scroll > 0 ? scroll : 0.f;
    // Reveal value runs 1 -> 0 over the scroll range; the banner is visible while it is non-zero.
    g.value = clamp01(1 - scroll / kUiGuildBannerScrollRange);
    g.visible = scroll < kUiGuildBannerScrollRange;
    const float top = header.top;
    g.image = {sidebar.left, top, sidebar.right, top + kUiGuildBannerHeight};
    // The container slides up by (1 - value) * kUiGuildBannerSlide while the
    // picture inside slides back down by the same amount: the picture stays
    // put and the container's lower edge uncovers the list.
    const float slide = (1 - g.value) * kUiGuildBannerSlide;
    g.clip = {sidebar.left, top, sidebar.right, maximum(top, top + kUiGuildBannerHeight - slide)};
    g.header = {sidebar.left, header.top, sidebar.right, header.bottom};
    const float strip = maximum(0.f, kUiGuildBannerScrollRange - scroll);
    g.hover = {sidebar.left, header.bottom, sidebar.right, header.bottom + strip};
    g.opacity = g.value;
    g.scrim = g.visible ? g.value : 0.f;
    g.glass = !g.visible ? 0.f : header_hovered ? 1.f : clamp01((1 - g.value) * 6);
    return g;
}

bool ui_guild_banner_animate(UiGuildBannerAnimation* s, u64 guild_id, bool eligible, bool visible, bool hovered,
                             bool gif_autoplay, u64 now_ms) noexcept {
    if (!s)
        return false;
    if (s->guild_id != guild_id)
        *s = {guild_id, 0, false, false};
    if (!guild_id || !eligible) {
        s->autoplay_until_ms = 0;
        s->was_visible = visible && guild_id;
        s->hovered = false;
        return false;
    }
    // Entering view starts the timed play; leaving it cancels it.
    if (visible && !s->was_visible && gif_autoplay)
        s->autoplay_until_ms = now_ms + kUiGuildBannerAutoplayMs;
    if (!visible)
        s->autoplay_until_ms = 0;
    s->was_visible = visible;
    hovered = hovered && visible;
    // Entering the hover strip takes over from the timer.
    if (hovered && !s->hovered)
        s->autoplay_until_ms = 0;
    s->hovered = hovered;
    if (s->autoplay_until_ms && now_ms >= s->autoplay_until_ms)
        s->autoplay_until_ms = 0;
    return visible && (hovered || s->autoplay_until_ms != 0);
}

u64 ui_guild_banner_next_wake(const UiGuildBannerAnimation& s, u64 now_ms) noexcept {
    return s.autoplay_until_ms > now_ms ? s.autoplay_until_ms : 0;
}
} // namespace maicord
