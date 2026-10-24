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
    // GIF provider hosts that favourites load from directly. Only the tiny
    // animated WebP renditions parse_catalog_preview_url derives (and Klipy's own
    // GIF/MP4 assets) are ever requested.
    catalog_tenor,
    catalog_giphy,
    catalog_klipy2, // static2.klipy.com, under catalog_klipy's rules.
    // i.scdn.co: Spotify album art of rich presence cards ("/image/<id>" as
    // JPEG), which the official client loads from there too.
    activity_art,
    // Resolved, credential-free YouTube media only. CDN client targets carry
    // the full HTTPS URL because Google assigns a per-stream subdomain.
    google_video,
    tiktok_video,
    twitch_video,
    spotify_preview,
    streamable_video,
    apple_music_preview,
    apple_music_video_preview,
    steam_video,
    twitch_vod,
    twitch_live,
    twitch_playlist,
    imgur_video,
    twitter_video,
    amazon_music_preview,
    youtube_live,
    youtube_playlist,
    pornhub_video,
    vimeo_video,
    soundcloud_audio,
};
inline constexpr char kActivityArtHost[] = "i.scdn.co";
// An activity art target: "/image/" and 1-64 ASCII letters or digits.
constexpr bool activity_art_target_valid(ConstSlice<u8> target) noexcept {
    constexpr char prefix[] = "/image/";
    constexpr u32 prefix_bytes = sizeof(prefix) - 1;
    if (!target.data || target.count <= prefix_bytes || target.count > prefix_bytes + 64)
        return false;
    for (u32 i = 0; i < target.count; ++i) {
        const u8 c = target[i];
        if (i < prefix_bytes ? c != static_cast<u8>(prefix[i])
                             : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            return false;
    }
    return true;
}
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
// They do not change the AttachmentUrlError that admission reports.
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
// Request target for the src of a synced GIF favourite; format selects the
// accepted shapes.
// webp: Tenor/Giphy media in any rendition (including mp4/webm VIDEO
// favourites), direct or behind the external proxy, map to the provider's tiny
// animated WebP. A Klipy .webp asset (static.klipy.com: no credentials, port or
// fragment; ?animated=true allowed) loads by its bare path. Discord attachment
// images and any other image behind the signed proxy
// (/external/<signature>/<scheme>/<host>/<path> on images-ext-N or
// media.discordapp.net) load as the proxy's animated WebP, fitted to
// width x height and capped at 160 px (0: 160). A raw URL on any other host is
// refused: the proxy is the only route to an arbitrary origin.
// gif: a direct Klipy .gif asset, optionally with ?format=webp&animated=true,
// fetched bare.
// mp4 (muted looping tiles): a Klipy .mp4 asset, a Discord attachment .mp4 or
// an .mp4 behind the proxy; Tenor/Giphy videos use their WebP rendition.
MediaUrlResult parse_catalog_preview_url(
    Utf8View url, Slice<u8> target_output,
    MediaAttachmentFormat format = MediaAttachmentFormat::webp, u32 width = 0, u32 height = 0) noexcept;
// The preview format for a stored GIF favourite src: a direct Klipy .gif is
// fetched as GIF, an MP4 the mp4 format admits plays as video, and every
// other shape (an IMAGE favourite's Klipy .webp among them) previews as an
// animated WebP. The src decides, not FavoriteGIF.format: IMAGE is stored for any
// .webp/.avif/.gif src and VIDEO only for media.
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
// An a_ hash adds &animated=true only when animate is set; otherwise the same
// .webp?size=N is the CDN's still.
CdnTargetResult write_guild_icon_cdn_target(u64 guild_id, Utf8View icon_hash, u32 size,
                                            Slice<u8> output, bool animate = true) noexcept;
CdnTargetResult write_user_avatar_cdn_target(u64 user_id, Utf8View avatar_hash, u32 size,
                                             Slice<u8> output) noexcept;
// Custom group DM icon: /channel-icons/{channel_id}/{hash}.webp?size=N.
CdnTargetResult write_channel_icon_cdn_target(u64 channel_id, Utf8View icon_hash, u32 size,
                                              Slice<u8> output) noexcept;

// Hash-addressed avatar/icon requests default to 128 px.
inline constexpr u32 kCdnIdentityDefaultSize = 128;
// Largest DIP extent at which shared user-avatar and group-icon identities are
// drawn (message authors and settings rows: 40; member, DM and voter rows: 32).
inline constexpr u32 kCdnIdentityDisplayDips = 40;
// Smallest documented power-of-two CDN size covering display_dips at dpi, at
// least 16 and at most kCdnIdentityDefaultSize. One size per DPI keeps a single
// shared atlas identity per avatar.
constexpr u32 cdn_image_size(u32 display_dips, u32 dpi) noexcept {
    const u64 pixels = (static_cast<u64>(display_dips) * (dpi ? dpi : 96u) + 95u) / 96u;
    u32 size = 16;
    while (size < pixels && size < kCdnIdentityDefaultSize)
        size <<= 1;
    return size;
}

// Sticker format types: PNG 1, APNG 2, Lottie 3, GIF 4.
// GIF stickers use media.discordapp.net; all other formats use the CDN host.
CdnTargetResult write_sticker_cdn_target(u64 sticker_id, u32 format_type,
                                         Slice<u8> output) noexcept;

Utf8View cdn_host_name(CdnHost host) noexcept;

} // namespace maicord
