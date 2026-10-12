#pragma once

#include <stddef.h>
#include "maicord/core/slice.hpp"
#include "maicord/media/asset.hpp"

// The account module carries the only copy of these functions. MaiCord.exe
// links forwarders to its AccountModuleCdnUrls entries, which fail like
// invalid input (empty host name, zero bytes, WebP format) while no module is
// loaded. guild_icon_hash_valid is its own object and stays in both images.
namespace maicord {

inline constexpr char kCdnHost[] = "cdn.discordapp.com";
inline constexpr char kMediaProxyHost[] = "media.discordapp.net";
inline constexpr char kImagesExt1Host[] = "images-ext-1.discordapp.net";
inline constexpr char kImagesExt2Host[] = "images-ext-2.discordapp.net";
inline constexpr char kCatalogKlipyHost[] = "static.klipy.com";
// Klipy's second static host: the same asset shapes as static.klipy.com.
inline constexpr char kCatalogKlipyStatic2Host[] = "static2.klipy.com";
inline constexpr u32 kCdnMaximumTargetBytes = 2u * 1024u;

enum class CdnHost : u8 {
    none,
    cdn,
    media,
    images_ext_1,
    images_ext_2,
    flag_assets,   // Only the fixed regional-flag metadata targets, never general media URLs.
    catalog_klipy, // Only typed catalog preview requests; excluded from ordinary message URLs.
    // GIF providers the official client loads favourites from directly
    // (isAllowedGifProviderUrl). Only the tiny animated WebP renditions
    // parse_catalog_preview_url derives (and Klipy's own GIF/MP4
    // assets) are ever requested.
    catalog_tenor,
    catalog_giphy,
    catalog_klipy2, // static2.klipy.com, under catalog_klipy's rules.
};
constexpr bool cdn_klipy_host(CdnHost host) noexcept {
    return host == CdnHost::catalog_klipy || host == CdnHost::catalog_klipy2;
}
constexpr bool cdn_catalog_provider(CdnHost host) noexcept {
    return cdn_klipy_host(host) || host == CdnHost::catalog_tenor ||
           host == CdnHost::catalog_giphy;
}

enum class CdnMediaFormat : u8 {
    png,
    webp,
    jpeg,
    source, // Resize without changing the encoding (notably animated GIF).
};

enum class CdnTargetError : u8 {
    none,
    invalid_input,
    unsupported_format,
    output_capacity,
};

struct CdnTargetResult {
    u32 bytes;
    CdnHost host;
    CdnTargetError error;

    constexpr bool ok() const noexcept {
        return error == CdnTargetError::none;
    }
};

enum class AttachmentUrlError : u8 {
    none,
    invalid_input,
    invalid_scheme,
    invalid_host,
    invalid_path,
    attachment_mismatch,
    invalid_query,
    invalid_timestamp,
    invalid_signature,
    output_capacity,
};

// Diagnostic categories never contain query names, values, or signed URLs.
// Admission still reports its existing error and preserves authority checks.
enum class AttachmentQueryError : u8 {
    none,
    empty_component,
    missing_equals,
    empty_name_or_value,
    duplicate_authority,
    invalid_dimension,
    unsupported_format,
    unsupported_quality,
    unsupported_parameter,
    missing_authority,
    animated_parameter,
    force_sdr_parameter,
    passthrough_parameter,
    aspect_ratio_parameter,
    backend_parameter,
};

struct AttachmentUrlResult {
    u64 issued_at_seconds;
    u64 expires_at_seconds;
    u32 target_bytes;
    CdnHost host;
    AttachmentUrlError error;
    bool ephemeral;
    AttachmentQueryError query_error;

    constexpr bool ok() const noexcept {
        return error == AttachmentUrlError::none;
    }
};

static_assert(sizeof(AttachmentUrlResult) == 24);
static_assert(offsetof(AttachmentUrlResult, query_error) == 23);

enum class MediaUrlError : u8 {
    none,
    invalid_input,
    invalid_scheme,
    invalid_host,
    invalid_target,
    output_capacity,
};

struct MediaUrlResult {
    u32 target_bytes;
    CdnHost host;
    MediaUrlError error;

    constexpr bool ok() const noexcept {
        return error == MediaUrlError::none;
    }
};

// Parses a Discord signed attachment URL into a credential-free HTTP
// origin-form target. This validates the documented origin, attachment path
// identity, timestamps, and hexadecimal signature representation. The hm
// value is authenticated by Discord's CDN, not by the client.
AttachmentUrlResult parse_signed_attachment_url(Utf8View url, u64 attachment_id,
                                                Slice<u8> target_output) noexcept;

// Extracts an origin-form target only from Discord's current CDN and media
// proxy origins. This path is used for embed proxy resources that do not use
// the signed /attachments URL shape. Arbitrary external hosts are rejected.
MediaUrlResult parse_media_url(Utf8View url, Slice<u8> target_output) noexcept;
// Exact origin-form target for a catalog provider host: Klipy .webp/.gif/.mp4
// assets, Tenor /<id>AAAA1/<slug>.webp, Giphy /media/<id>/200w.webp.
bool catalog_provider_target_valid(CdnHost host, ConstSlice<u8> target) noexcept;
bool sticker_json_target_valid(ConstSlice<u8> target) noexcept;
// Exactly /soundboard-sounds/{id}.
bool soundboard_target_valid(ConstSlice<u8> target) noexcept;
// WebP previews also accept the src shapes GIF favourites synced from the
// official client carry: Tenor/Giphy media in any rendition (including
// mp4/webm VIDEO favourites), direct or behind Discord's external proxy, map
// to the provider's tiny animated WebP; a Klipy .webp asset (Discord's
// static.klipy.com filter: no credentials, port or fragment) loads by its bare
// path, also when stored with the ?animated=true the official client appends
// to a proxied gifv embed's thumbnail; Discord attachment images and any other external image behind Discord's
// signed proxy (/external/<signature>/<scheme>/<host>/<path> on images-ext-N
// or media.discordapp.net, e.g. a Klipy gifv thumbnail or an animated WebP on
// its own host) load as the proxy's animated WebP sized to fit width x height
// (0: 160 px). A raw URL on any other host is refused: Discord's proxy, not
// the client, is the only route to an arbitrary origin.
// GIF previews accept a direct Klipy .gif asset (the official picker's gif_src,
// or with ?format=webp&animated=true from a proxied embed), fetched bare.
// MP4 previews (VIDEO favourites the picker plays as muted looping tiles)
// accept a Klipy .mp4 asset, a Discord attachment .mp4 and an .mp4 behind the
// external proxy; Tenor/Giphy videos keep their WebP rendition.
MediaUrlResult parse_catalog_preview_url(
    Utf8View url, Slice<u8> target_output,
    MediaAttachmentFormat format = MediaAttachmentFormat::webp, u32 width = 0, u32 height = 0) noexcept;
// The preview format for a stored GIF favourite src: a direct Klipy .gif is
// fetched as GIF, an MP4 the mp4 format admits plays as video, and every
// other shape (an IMAGE favourite's Klipy .webp among them) previews as an
// animated WebP. The src decides rather than FavoriteGIF.format: the official
// client stores IMAGE for any .webp/.avif/.gif src and VIDEO only for media.
MediaAttachmentFormat catalog_favorite_format(Utf8View src) noexcept;
// Loggable fields of a media URL for diagnostics: " host=<host> ext=<ext>",
// plus " via=<host>" for the origin behind Discord's /external/ proxy. Paths,
// queries and fragments (signed tokens) are never written. Returns the bytes
// written (truncated to output, no terminator).
u32 media_url_log_fields(Utf8View url, Slice<char> output) noexcept;

// Rewrites a validated Discord origin-form image target for a bounded decode.
// Generic media uses the exact aspect-preserving width/height selected by the
// caller. CDN avatar/icon endpoints retain their native `size` contract and
// origin so Discord does not silently fall back to the 128px default.
CdnTargetResult write_media_proxy_target(ConstSlice<u8> source_target,
                                         CdnHost source_host, u32 target_width,
                                         u32 target_height, CdnMediaFormat format,
                                         Slice<u8> output, bool animated = false) noexcept;

// Discord documents powers of two from 16 through 4096 for resizable image
// endpoints. Emoji are always requested as WebP; animated WebP adds the
// documented query flag. Static responses may use either VP8 or VP8L.
CdnTargetResult write_custom_emoji_cdn_target(u64 emoji_id, bool animated, u32 size,
                                              Slice<u8> output) noexcept;

// Guild icon hashes are the 32 hexadecimal hash returned by the guild object,
// optionally prefixed by `a_` for an animated asset. Both static and animated
// assets use WebP; the hash controls animation identity, not decoder choice.
bool guild_icon_hash_valid(Utf8View icon_hash) noexcept;
u32 cdn_guild_icon_variant(Utf8View icon_hash) noexcept;
// Discord's getGuildIconURL: an a_ hash adds &animated=true only when it may
// animate (canAnimate); otherwise the same .webp?size=N is the CDN's still.
CdnTargetResult write_guild_icon_cdn_target(u64 guild_id, Utf8View icon_hash, u32 size,
                                            Slice<u8> output, bool animate = true) noexcept;
CdnTargetResult write_user_avatar_cdn_target(u64 user_id, Utf8View avatar_hash, u32 size,
                                             Slice<u8> output) noexcept;
// Custom group DM icon: /channel-icons/{channel_id}/{hash}.webp?size=N.
CdnTargetResult write_channel_icon_cdn_target(u64 channel_id, Utf8View icon_hash, u32 size,
                                              Slice<u8> output) noexcept;

// Hash-addressed avatar/icon requests default to the historical 128px.
inline constexpr u32 kCdnIdentityDefaultSize = 128;
// Largest DIP extent at which shared user-avatar and group-icon identities are
// drawn (message authors and settings rows: 40; member, DM and voter rows: 32).
inline constexpr u32 kCdnIdentityDisplayDips = 40;
// Smallest documented power-of-two CDN size covering display_dips at dpi, at
// least 16 and at most maximum (a power of two, defaulting to 128). One size per
// DPI keeps a single shared atlas identity per avatar.
constexpr u32 cdn_image_size(u32 display_dips, u32 dpi,
                             u32 maximum = kCdnIdentityDefaultSize) noexcept {
    const u64 pixels = (static_cast<u64>(display_dips) * (dpi ? dpi : 96u) + 95u) / 96u;
    u32 size = 16;
    while (size < pixels && size < maximum && size < 4'096)
        size <<= 1;
    return size;
}

// Sticker format types are the official PNG/APNG/Lottie/GIF values 1..4.
// GIF stickers use media.discordapp.net; all other formats use the CDN host.
CdnTargetResult write_sticker_cdn_target(u64 sticker_id, u32 format_type,
                                         Slice<u8> output) noexcept;

Utf8View cdn_host_name(CdnHost host) noexcept;

} // namespace maicord
