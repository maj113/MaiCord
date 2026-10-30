#pragma once

#include "maicord/protocol/cdn_url.hpp"
#include "maicord/media/encoded_video_limits.hpp"

namespace maicord {

inline constexpr u32 kCdnMaximumRequestHeadBytes = kCdnMaximumTargetBytes + 768;
inline constexpr u32 kCdnMaximumResponseHeaderBytes = 16u * 1024u;
inline constexpr u32 kCdnMaximumResponseFields = 32;
inline constexpr u32 kCdnMaximumEncodedBodyBytes = 16u * 1024u * 1024u;
inline constexpr u32 kCdnMaximumEtagBytes = 96;
inline constexpr u64 kCdnMaximumFreshnessSeconds = 7ull * 24ull * 60ull * 60ull;

enum class CdnImageType : u8 {
    png,
    webp,
    gif,
    jpeg,
    mp4,
    lottie_json,
    ogg_opus,
    mp3,
    wav,
    m4a,
    // /soundboard-sounds/{id}: MP3 or Ogg Opus, told apart by content.
    soundboard_sound,
    hls_playlist,
    mpeg_ts,
};
// A catalog provider request: animated WebP from every provider, plus a Klipy
// .gif asset (a GIF favourite's stored gif_src) or .mp4 asset (a VIDEO
// favourite without one). Type and extension agree.
bool catalog_provider_request_valid(CdnHost host, CdnImageType type, ConstSlice<u8> target) noexcept;
// An origin-form request target (one leading "/", visible ASCII, no '#' or '\\') and an ETag value.
bool cdn_target_valid(ConstSlice<u8> target) noexcept;
bool cdn_etag_valid(ConstSlice<u8> etag) noexcept;
constexpr u32 cdn_encoded_body_limit(CdnImageType type) noexcept {
    return type == CdnImageType::mp4                 ? kMediaMaximumEncodedVideoBytes
           : type == CdnImageType::mpeg_ts            ? 8u * 1024u * 1024u
           : type == CdnImageType::hls_playlist       ? 64u * 1024u
           : type == CdnImageType::lottie_json       ? 1024u * 1024u
           : type == CdnImageType::soundboard_sound  ? 1024u * 1024u
                                                     : kCdnMaximumEncodedBodyBytes;
}

enum class CdnHttpError : u8 {
    none,
    invalid_input,
    invalid_target,
    output_capacity,
    invalid_etag,
    incomplete_head,
    malformed_head,
    duplicate_field,
    invalid_content_type,
    invalid_content_encoding,
    invalid_cache_control,
    invalid_age,
    invalid_location,
    unsupported_status,
    body_too_large,
    body_size_mismatch,
    invalid_content_range,
};

// One HTTP Range request (RFC 9110 14.1.2). bytes == 0 means a whole-entity GET.
struct CdnByteRange {
    u64 first;
    u32 bytes;
};

struct CdnRequestResult {
    u32 bytes;
    CdnHttpError error;

    constexpr bool ok() const noexcept {
        return error == CdnHttpError::none;
    }
};

// Writes a transactional, credential-free GET head. The only conditional
// state accepted is a bounded ETag; Authorization and Cookie cannot enter this
// API.
CdnRequestResult write_static_cdn_get_request(CdnImageType image_type, CdnHost host, ConstSlice<u8> target,
                                              ConstSlice<u8> etag, Slice<u8> output,
                                              CdnByteRange range = {}) noexcept;
// A static-host range is accepted only for MP4/M4A and never with an ETag.
// Native MPEG-TS uses the separately admitted provider request path.

struct CdnRedirectResult {
    CdnHost host;
    u32 target_bytes;
    CdnHttpError error;

    constexpr bool ok() const noexcept {
        return error == CdnHttpError::none;
    }
};

// Accepts only an origin-form target on the current host or an absolute HTTPS
// URL on one of Discord's two documented media origins.
CdnRedirectResult parse_cdn_redirect(CdnHost current_host, ConstSlice<u8> location,
                                     Slice<u8> target_output) noexcept;

enum class CdnResponseDisposition : u8 {
    none,
    accept_body,
    not_modified,
    redirect,
    // 206 for a range request: body holds [range_first, range_first + body_bytes).
    accept_partial,
    // 403/410 for a range request: the signed URL must be refreshed.
    source_expired,
};

struct CdnResponseResult {
    ConstSlice<u8> etag;
    ConstSlice<u8> location;
    u64 expires_at_seconds;
    u32 header_bytes;
    u32 body_bytes;
    u16 status_code;
    CdnResponseDisposition disposition;
    CdnHttpError error;
    bool store_allowed;
    bool must_revalidate;
    // Range responses: complete entity length and the first body byte's offset.
    // A 200 answer to a range request carries the whole entity at offset 0.
    u64 range_first;
    u64 entity_bytes;

    constexpr bool ok() const noexcept {
        return error == CdnHttpError::none;
    }
};

// Parses and validates a fully received response head plus the decoded entity
// byte count. The returned slices borrow response_head storage.
CdnResponseResult validate_static_image_response(CdnImageType image_type, ConstSlice<u8> response_head,
                                                 u32 decoded_body_bytes, u64 now_seconds,
                                                 CdnHost host = CdnHost::none) noexcept;
// Range answers: 206 needs a single "bytes first-last/length" Content-Range
// starting at the requested offset, within the request and matching the body.
// A server that ignores Range answers 200 with the whole (bounded) entity.
CdnResponseResult validate_cdn_range_response(CdnImageType image_type, ConstSlice<u8> response_head,
                                              u32 decoded_body_bytes, CdnByteRange requested,
                                              u64 now_seconds, CdnHost host = CdnHost::none) noexcept;

} // namespace maicord
