#include "maicord/protocol/cdn_url.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {

bool image_size_valid(u32 size) noexcept {
    return size >= 16 && size <= 4'096 && (size & (size - 1)) == 0;
}
u32 literal_length(const char* literal) noexcept {
    u32 bytes = 0;
    while (literal[bytes] != 0) {
        ++bytes;
    }
    return bytes;
}

bool ascii_equal(const char* value, u32 bytes, const char* literal) noexcept {
    if (bytes != literal_length(literal)) {
        return false;
    }
    for (u32 index = 0; index < bytes; ++index) {
        if (value[index] != literal[index]) {
            return false;
        }
    }
    return true;
}

bool bytes_begin_with(ConstSlice<u8> value, u32 bytes, const char* literal) noexcept {
    if (bytes > value.count) {
        return false;
    }
    const u32 literal_bytes = literal_length(literal);
    if (literal_bytes > bytes) {
        return false;
    }
    for (u32 index = 0; index < literal_bytes; ++index) {
        if (value[index] != static_cast<u8>(literal[index])) {
            return false;
        }
    }
    return true;
}

// Canonical decimal: no leading zero other than "0" itself.
bool parse_decimal(const char* value, u32 bytes, u64* output) noexcept {
    return bytes != 0 && (bytes == 1 || value[0] != '0') && decimal_parse({value, bytes}, output);
}

bool parse_hexadecimal(const char* value, u32 bytes, u64* output) noexcept {
    if (bytes == 0 || bytes > 16) {
        return false;
    }
    u64 parsed = 0;
    for (u32 index = 0; index < bytes; ++index) {
        const i32 digit = ascii_hex_value(value[index]);
        if (digit < 0) {
            return false;
        }
        parsed = (parsed << 4) | static_cast<u64>(digit);
    }
    *output = parsed;
    return true;
}

AttachmentUrlResult attachment_url_failure(
    AttachmentUrlError error,
    AttachmentQueryError query_error = AttachmentQueryError::none) noexcept {
    AttachmentUrlResult result{};
    result.error = error;
    result.query_error = query_error;
    return result;
}

MediaUrlResult media_url_failure(MediaUrlError error) noexcept {
    MediaUrlResult result{};
    result.error = error;
    return result;
}

CdnTargetResult failure(CdnTargetError error) noexcept {
    return CdnTargetResult{0, CdnHost::none, error};
}

CdnTargetResult publish_target(ConstSlice<u8> target, CdnHost host,
                               Slice<u8> output) noexcept {
    if (target.count > output.count) {
        return failure(CdnTargetError::output_capacity);
    }
    copy_bytes(output.data, target.data, target.count);
    return CdnTargetResult{target.count, host, CdnTargetError::none};
}

} // namespace

AttachmentUrlResult parse_signed_attachment_url(Utf8View url, u64 attachment_id,
                                                Slice<u8> target_output) noexcept {
    if (!url.data || url.bytes == 0 || attachment_id == 0 ||
        (!target_output.data && target_output.count != 0)) {
        return attachment_url_failure(AttachmentUrlError::invalid_input);
    }
    constexpr char scheme[] = "https://";
    constexpr u32 scheme_bytes = sizeof(scheme) - 1;
    if (url.bytes <= scheme_bytes || !ascii_equal(url.data, scheme_bytes, scheme)) {
        return attachment_url_failure(AttachmentUrlError::invalid_scheme);
    }
    u32 path_begin = scheme_bytes;
    while (path_begin < url.bytes && url.data[path_begin] != '/') {
        ++path_begin;
    }
    CdnHost parsed_host = CdnHost::none;
    if (path_begin != url.bytes &&
        ascii_equal(url.data + scheme_bytes, path_begin - scheme_bytes, kCdnHost)) {
        parsed_host = CdnHost::cdn;
    } else if (path_begin != url.bytes &&
               ascii_equal(url.data + scheme_bytes, path_begin - scheme_bytes, kMediaProxyHost)) {
        parsed_host = CdnHost::media;
    } else {
        return attachment_url_failure(AttachmentUrlError::invalid_host);
    }
    const u32 target_bytes = url.bytes - path_begin;
    if (target_bytes == 0 || target_bytes > kCdnMaximumTargetBytes) {
        return attachment_url_failure(AttachmentUrlError::invalid_path);
    }
    for (u32 index = path_begin; index < url.bytes; ++index) {
        const u8 current = static_cast<u8>(url.data[index]);
        if (current <= 0x20 || current > 0x7E || current == '#' || current == '\\') {
            return attachment_url_failure(AttachmentUrlError::invalid_path);
        }
    }

    constexpr char attachment_prefix[] = "/attachments/";
    constexpr char ephemeral_prefix[] = "/ephemeral-attachments/";
    constexpr u32 attachment_prefix_bytes = sizeof(attachment_prefix) - 1;
    constexpr u32 ephemeral_prefix_bytes = sizeof(ephemeral_prefix) - 1;
    bool ephemeral = false;
    u32 offset = path_begin;
    if (target_bytes > attachment_prefix_bytes &&
        ascii_equal(url.data + offset, attachment_prefix_bytes, attachment_prefix)) {
        offset += attachment_prefix_bytes;
    } else if (target_bytes > ephemeral_prefix_bytes &&
               ascii_equal(url.data + offset, ephemeral_prefix_bytes, ephemeral_prefix)) {
        offset += ephemeral_prefix_bytes;
        ephemeral = true;
    } else {
        return attachment_url_failure(AttachmentUrlError::invalid_path);
    }

    u32 slash = offset;
    while (slash < url.bytes && url.data[slash] != '/') {
        ++slash;
    }
    u64 owner_id = 0;
    if (slash == url.bytes || !parse_decimal(url.data + offset, slash - offset, &owner_id) ||
        owner_id == 0) {
        return attachment_url_failure(AttachmentUrlError::invalid_path);
    }
    offset = slash + 1;
    slash = offset;
    while (slash < url.bytes && url.data[slash] != '/') {
        ++slash;
    }
    u64 parsed_attachment_id = 0;
    if (slash == url.bytes || !parse_decimal(url.data + offset, slash - offset, &parsed_attachment_id)) {
        return attachment_url_failure(AttachmentUrlError::invalid_path);
    }
    if (parsed_attachment_id != attachment_id) {
        return attachment_url_failure(AttachmentUrlError::attachment_mismatch);
    }
    offset = slash + 1;
    u32 query = offset;
    while (query < url.bytes && url.data[query] != '?') {
        if (url.data[query] == '/') {
            return attachment_url_failure(AttachmentUrlError::invalid_path);
        }
        if (url.data[query] == '%') {
            if (query + 2 >= url.bytes || !ascii_is_hex_digit(url.data[query + 1]) ||
                !ascii_is_hex_digit(url.data[query + 2])) {
                return attachment_url_failure(AttachmentUrlError::invalid_path);
            }
            query += 3;
        } else {
            ++query;
        }
    }
    if (query == offset) {
        return attachment_url_failure(AttachmentUrlError::invalid_path);
    }

    // Unsigned attachment paths are not fetchable but still identify the file in
    // older message payloads. Return the validated target already expired, so the
    // account layer requests a fresh message record instead of an unauthenticated GET.
    AttachmentUrlResult result{};
    result.target_bytes = target_bytes;
    result.host = parsed_host;
    result.ephemeral = ephemeral;
    if (query == url.bytes) {
        if (target_bytes > target_output.count) {
            return attachment_url_failure(AttachmentUrlError::output_capacity);
        }
        copy_bytes(target_output.data, url.data + path_begin, target_bytes);
        return result;
    }
    if (query + 1 == url.bytes) {
        return attachment_url_failure(AttachmentUrlError::invalid_query,
                                      AttachmentQueryError::empty_component);
    }

    bool has_expiry = false;
    bool has_issued = false;
    bool has_signature = false;
    bool has_backend = false;
    u64 expires_at = 0;
    u64 issued_at = 0;
    offset = query + 1;
    while (offset < url.bytes) {
        u32 end = offset;
        while (end < url.bytes && url.data[end] != '&') {
            ++end;
        }
        if (end == offset) {
            return attachment_url_failure(AttachmentUrlError::invalid_query,
                                          AttachmentQueryError::empty_component);
        }
        u32 equals = offset;
        while (equals < end && url.data[equals] != '=') {
            ++equals;
        }
        if (equals == end) {
            return attachment_url_failure(AttachmentUrlError::invalid_query,
                                          AttachmentQueryError::missing_equals);
        }
        // Some attachment URLs carry an empty `=` media-proxy marker. It grants
        // nothing and is dropped from the canonical target.
        if (equals == offset && equals + 1 == end) {
            offset = end < url.bytes ? end + 1 : end;
            continue;
        }
        if (equals == offset || equals + 1 == end) {
            return attachment_url_failure(AttachmentUrlError::invalid_query,
                                          AttachmentQueryError::empty_name_or_value);
        }
        const char* name = url.data + offset;
        const u32 name_bytes = equals - offset;
        const char* value = url.data + equals + 1;
        const u32 value_bytes = end - equals - 1;
        if (ascii_equal(name, name_bytes, "ex")) {
            if (has_expiry || !parse_hexadecimal(value, value_bytes, &expires_at)) {
                return attachment_url_failure(has_expiry ? AttachmentUrlError::invalid_query
                                                         : AttachmentUrlError::invalid_timestamp,
                                              has_expiry ? AttachmentQueryError::duplicate_authority
                                                         : AttachmentQueryError::none);
            }
            has_expiry = true;
        } else if (ascii_equal(name, name_bytes, "is")) {
            if (has_issued || !parse_hexadecimal(value, value_bytes, &issued_at)) {
                return attachment_url_failure(has_issued ? AttachmentUrlError::invalid_query
                                                         : AttachmentUrlError::invalid_timestamp,
                                              has_issued ? AttachmentQueryError::duplicate_authority
                                                         : AttachmentQueryError::none);
            }
            has_issued = true;
        } else if (ascii_equal(name, name_bytes, "hm")) {
            if (has_signature || value_bytes != 64) {
                return attachment_url_failure(has_signature ? AttachmentUrlError::invalid_query
                                                            : AttachmentUrlError::invalid_signature,
                                              has_signature
                                                  ? AttachmentQueryError::duplicate_authority
                                                  : AttachmentQueryError::none);
            }
            for (u32 index = 0; index < value_bytes; ++index) {
                if (!ascii_is_hex_digit(value[index])) {
                    return attachment_url_failure(AttachmentUrlError::invalid_signature);
                }
            }
            has_signature = true;
        } else if (ascii_equal(name, name_bytes, "backend")) {
            // Routing hint selecting B2 storage; kept in the target. Any other
            // backend value or a repeat is refused.
            if (has_backend || !ascii_equal(value, value_bytes, "b2")) {
                return attachment_url_failure(AttachmentUrlError::invalid_query,
                                              AttachmentQueryError::backend_parameter);
            }
            has_backend = true;
        } else if (ascii_equal(name, name_bytes, "width") || ascii_equal(name, name_bytes, "height") ||
                   ascii_equal(name, name_bytes, "size")) {
            u64 dimension = 0;
            if (!parse_decimal(value, value_bytes, &dimension) || dimension == 0 || dimension > 4'096) {
                return attachment_url_failure(AttachmentUrlError::invalid_query,
                                              AttachmentQueryError::invalid_dimension);
            }
        } else if (ascii_equal(name, name_bytes, "format")) {
            if (!ascii_equal(value, value_bytes, "png") && !ascii_equal(value, value_bytes, "webp") &&
                !ascii_equal(value, value_bytes, "jpeg") && !ascii_equal(value, value_bytes, "jpg") &&
                !ascii_equal(value, value_bytes, "avif")) {
                return attachment_url_failure(AttachmentUrlError::invalid_query,
                                              AttachmentQueryError::unsupported_format);
            }
        } else if (ascii_equal(name, name_bytes, "quality")) {
            if (!ascii_equal(value, value_bytes, "lossless")) {
                return attachment_url_failure(AttachmentUrlError::invalid_query,
                                              AttachmentQueryError::unsupported_quality);
            }
        } else if (ascii_equal(name, name_bytes, "animated")) {
            // Boolean animated-preview hint; carries no signed-attachment authority.
            if (!ascii_equal(value, value_bytes, "true") && !ascii_equal(value, value_bytes, "false")) {
                return attachment_url_failure(AttachmentUrlError::invalid_query,
                                              AttachmentQueryError::animated_parameter);
            }
        } else {
            const auto category = ascii_equal(name, name_bytes, "force_sdr")
                                      ? AttachmentQueryError::force_sdr_parameter
                                  : ascii_equal(name, name_bytes, "passthrough")
                                      ? AttachmentQueryError::passthrough_parameter
                                  : ascii_equal(name, name_bytes, "keep_aspect_ratio")
                                      ? AttachmentQueryError::aspect_ratio_parameter
                                      : AttachmentQueryError::unsupported_parameter;
            return attachment_url_failure(AttachmentUrlError::invalid_query, category);
        }
        offset = end < url.bytes ? end + 1 : end;
    }
    if (!has_expiry || !has_issued || !has_signature) {
        return attachment_url_failure(AttachmentUrlError::invalid_query,
                                      AttachmentQueryError::missing_authority);
    }
    if (issued_at > expires_at) {
        return attachment_url_failure(AttachmentUrlError::invalid_timestamp);
    }
    if (target_bytes > target_output.count) {
        return attachment_url_failure(AttachmentUrlError::output_capacity);
    }
    copy_bytes(target_output.data, url.data + path_begin, target_bytes);
    result.issued_at_seconds = issued_at;
    result.expires_at_seconds = expires_at;
    return result;
}

MediaUrlResult parse_media_url(Utf8View url, Slice<u8> target_output) noexcept {
    if (!url.data || url.bytes == 0 || (!target_output.data && target_output.count != 0)) {
        return media_url_failure(MediaUrlError::invalid_input);
    }
    constexpr char scheme[] = "https://";
    constexpr u32 scheme_bytes = sizeof(scheme) - 1;
    if (url.bytes <= scheme_bytes || !ascii_equal(url.data, scheme_bytes, scheme)) {
        return media_url_failure(MediaUrlError::invalid_scheme);
    }
    u32 path_begin = scheme_bytes;
    while (path_begin < url.bytes && url.data[path_begin] != '/') {
        ++path_begin;
    }
    if (path_begin == url.bytes) {
        return media_url_failure(MediaUrlError::invalid_target);
    }
    CdnHost host = CdnHost::none;
    const u32 host_bytes = path_begin - scheme_bytes;
    if (ascii_equal(url.data + scheme_bytes, host_bytes, kCdnHost)) {
        host = CdnHost::cdn;
    } else if (ascii_equal(url.data + scheme_bytes, host_bytes, kMediaProxyHost)) {
        host = CdnHost::media;
    } else if (ascii_equal(url.data + scheme_bytes, host_bytes, kImagesExt1Host)) {
        host = CdnHost::images_ext_1;
    } else if (ascii_equal(url.data + scheme_bytes, host_bytes, kImagesExt2Host)) {
        host = CdnHost::images_ext_2;
    } else {
        return media_url_failure(MediaUrlError::invalid_host);
    }
    const u32 target_bytes = url.bytes - path_begin;
    if (target_bytes == 0 || target_bytes > kCdnMaximumTargetBytes) {
        return media_url_failure(MediaUrlError::invalid_target);
    }
    for (u32 index = path_begin; index < url.bytes; ++index) {
        const u8 current = static_cast<u8>(url.data[index]);
        if (current <= 0x20 || current > 0x7E || current == '#' || current == '\\') {
            return media_url_failure(MediaUrlError::invalid_target);
        }
        if (current == '%' &&
            (index + 2 >= url.bytes || !ascii_is_hex_digit(url.data[index + 1]) ||
             !ascii_is_hex_digit(url.data[index + 2]))) {
            return media_url_failure(MediaUrlError::invalid_target);
        }
    }
    if (target_bytes > target_output.count) {
        return media_url_failure(MediaUrlError::output_capacity);
    }
    copy_bytes(target_output.data, url.data + path_begin, target_bytes);
    return MediaUrlResult{target_bytes, host, MediaUrlError::none};
}

namespace {
// Klipy asset path: [a-zA-Z0-9/_-]+ plus the given extension, with no empty
// segments, other dots or escapes.
bool klipy_asset_valid(ConstSlice<u8> target, const char* suffix, u32 suffix_bytes) noexcept {
    if (!target.data || target.count <= 1 + suffix_bytes || target.count > kCdnMaximumTargetBytes ||
        target[0] != '/' ||
        !ascii_equal(reinterpret_cast<const char*>(target.data + target.count - suffix_bytes), suffix_bytes,
                     suffix))
        return false;
    for (u32 index = 1; index < target.count - suffix_bytes; ++index) {
        const u8 value = target[index];
        if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '/') ||
            (value == '/' && target[index - 1] == '/'))
            return false;
    }
    return target[target.count - suffix_bytes - 1] != '/';
}
// A stored favourite's Klipy src: the asset path, alone or with the query a
// favourite of proxied media (a Klipy gifv embed) appends to its .webp/.avif/.gif
// source: format=webp (GIF/AVIF only) then animated=true, each at most once.
// Klipy's CDN serves the same asset whatever the query, so callers fetch the
// bare path. Returns the path's length, 0 for any other shape or parameter.
u32 klipy_favourite_path(ConstSlice<u8> target, const char* suffix, u32 suffix_bytes) noexcept {
    u32 query = 0;
    while (query < target.count && target[query] != '?')
        ++query;
    if (!klipy_asset_valid({target.data, query}, suffix, suffix_bytes))
        return 0;
    if (query == target.count)
        return query;
    u32 seen = 0, cursor = query + 1;
    if (cursor == target.count)
        return 0;
    while (cursor < target.count) {
        u32 end = cursor;
        while (end < target.count && target[end] != '&')
            ++end;
        const char* field = reinterpret_cast<const char*>(target.data + cursor);
        const u32 bit = ascii_equal(field, end - cursor, "format=webp")     ? 1u
                        : ascii_equal(field, end - cursor, "animated=true") ? 2u
                                                                            : 0u;
        if (!bit || (seen & bit) || end + 1 == target.count)
            return 0;
        seen |= bit;
        cursor = end + 1;
    }
    return query;
}
// The Klipy static origin ("https://static.klipy.com" or its second host
// static2) a URL starts with, and its length; none for any other URL. The
// asset filter then requires the path's leading '/'.
struct KlipyOrigin {
    CdnHost host;
    u32 bytes;
};
KlipyOrigin klipy_origin(Utf8View url) noexcept {
    constexpr char first[] = "https://static.klipy.com";
    constexpr char second[] = "https://static2.klipy.com";
    if (url.data && url.bytes > sizeof(first) - 1 && ascii_equal(url.data, sizeof(first) - 1, first))
        return {CdnHost::catalog_klipy, sizeof(first) - 1};
    if (url.data && url.bytes > sizeof(second) - 1 && ascii_equal(url.data, sizeof(second) - 1, second))
        return {CdnHost::catalog_klipy2, sizeof(second) - 1};
    return {CdnHost::none, 0};
}
// Query bits of a Klipy asset behind Discord's media proxy (1 format=webp,
// 2 animated=true, 4/8 width/height up to 512, 16 keep_aspect_ratio), or ~0u
// for any other path, parameter, value, duplicate or empty trailing field.
u32 catalog_proxy_query_bits(ConstSlice<u8> target) noexcept {
    constexpr u32 invalid = ~0u;
    if (!bytes_begin_with(target, target.count, "/external/"))
        return invalid;
    u32 cursor = 10;
    const u32 token_begin = cursor;
    while (cursor < target.count && target[cursor] != '/') {
        const u8 c = target[cursor++];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '-'))
            return invalid;
    }
    constexpr char provider[] = "/https/static.klipy.com";
    constexpr char provider2[] = "/https/static2.klipy.com";
    if (cursor == token_begin || cursor - token_begin > 128)
        return invalid;
    const ConstSlice<u8> wrapped{target.data + cursor, target.count - cursor};
    if (bytes_begin_with(wrapped, wrapped.count, provider))
        cursor += sizeof(provider) - 1;
    else if (bytes_begin_with(wrapped, wrapped.count, provider2))
        cursor += sizeof(provider2) - 1;
    else
        return invalid;
    u32 query = cursor;
    while (query < target.count && target[query] != '?')
        ++query;
    const ConstSlice<u8> provider_target{target.data + cursor, query - cursor};
    if (!klipy_asset_valid(provider_target, ".gif", 4) && !klipy_asset_valid(provider_target, ".webp", 5))
        return invalid;
    // The proxy's native output format takes precedence over the wrapped
    // provider filename. Keep requests bounded.
    u32 seen = 0;
    cursor = query + 1;
    while (cursor < target.count) {
        u32 end = cursor;
        while (end < target.count && target[end] != '&')
            ++end;
        u32 equals = cursor;
        while (equals < end && target[equals] != '=')
            ++equals;
        const char* name = reinterpret_cast<const char*>(target.data + cursor);
        const u32 name_bytes = equals - cursor;
        const char* value = reinterpret_cast<const char*>(target.data + equals + (equals < end));
        const u32 value_bytes = equals < end ? end - equals - 1 : 0;
        u32 bit = 0;
        bool valid = false;
        if (ascii_equal(name, name_bytes, "format")) {
            bit = 1;
            valid = ascii_equal(value, value_bytes, "webp");
        } else if (ascii_equal(name, name_bytes, "animated")) {
            bit = 2;
            valid = ascii_equal(value, value_bytes, "true");
        } else if (ascii_equal(name, name_bytes, "width") || ascii_equal(name, name_bytes, "height")) {
            bit = ascii_equal(name, name_bytes, "width") ? 4 : 8;
            u64 dimension = 0;
            valid = parse_decimal(value, value_bytes, &dimension) && dimension && dimension <= 512;
        } else if (ascii_equal(name, name_bytes, "keep_aspect_ratio")) {
            bit = 16;
            valid = ascii_equal(value, value_bytes, "true") || ascii_equal(value, value_bytes, "false");
        }
        if (!valid || (seen & bit) || (end < target.count && end + 1 == target.count))
            return invalid;
        seen |= bit;
        cursor = end < target.count ? end + 1 : end;
    }
    return seen;
}
// Discord's category proxy: every one of format, animated, width and height.
bool catalog_proxy_target_valid(ConstSlice<u8> target) noexcept {
    const u32 bits = catalog_proxy_query_bits(target);
    return bits != ~0u && (bits & 15) == 15;
}
} // namespace

bool sticker_json_target_valid(ConstSlice<u8> target) noexcept {
    if (!target.data || target.count <= 15 || target.count > kCdnMaximumTargetBytes ||
        !bytes_begin_with(target, target.count, "/stickers/") ||
        !ascii_equal(reinterpret_cast<const char*>(target.data + target.count - 5), 5, ".json"))
        return false;
    u64 sticker = 0;
    return parse_decimal(reinterpret_cast<const char*>(target.data + 10), target.count - 15, &sticker) &&
           sticker != 0;
}
bool soundboard_target_valid(ConstSlice<u8> target) noexcept {
    constexpr u32 prefix = sizeof("/soundboard-sounds/") - 1;
    if (!target.data || target.count <= prefix || target.count > prefix + 20 ||
        !bytes_begin_with(target, target.count, "/soundboard-sounds/"))
        return false;
    u64 sound = 0;
    return parse_decimal(reinterpret_cast<const char*>(target.data + prefix), target.count - prefix, &sound) &&
           sound != 0;
}

namespace {
bool gif_id_byte(u8 c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
bool slug_byte(u8 c) noexcept { return gif_id_byte(c) || c == '-' || c == '_'; }
// /<11-byte id>AAAA1/<slug>.webp
bool tenor_target_valid(ConstSlice<u8> target) noexcept {
    if (!target.data || target.count < 1 + 16 + 1 + 1 + 5 || target.count > 1 + 16 + 1 + 128 + 5 || target[0] != '/' ||
        target[17] != '/' || !bytes_equal(target.data + 12, "AAAA1", 5) ||
        !bytes_equal(target.data + target.count - 5, ".webp", 5))
        return false;
    for (u32 i = 1; i < 12; ++i)
        if (!slug_byte(target[i])) return false;
    for (u32 i = 18; i < target.count - 5; ++i)
        if (!slug_byte(target[i])) return false;
    return true;
}
// /media/<id>/200w.webp
bool giphy_target_valid(ConstSlice<u8> target) noexcept {
    constexpr u32 prefix = 7, suffix = 10;
    if (!target.data || target.count <= prefix + suffix || target.count > prefix + 64 + suffix ||
        !bytes_equal(target.data, "/media/", prefix) || !bytes_equal(target.data + target.count - suffix, "/200w.webp", suffix))
        return false;
    for (u32 i = prefix; i < target.count - suffix; ++i)
        if (!gif_id_byte(target[i])) return false;
    return true;
}
// Splits https://<host>/<path> (no port, credentials, query or fragment kept).
bool split_https(const char* url, u32 bytes, const char** host, u32* host_bytes, const char** path, u32* path_bytes) noexcept {
    constexpr char scheme[] = "https://";
    if (bytes <= 8 || !ascii_equal(url, 8, scheme)) return false;
    u32 slash = 8;
    while (slash < bytes && url[slash] != '/') ++slash;
    if (slash == bytes) return false;
    u32 end = slash;
    while (end < bytes && url[end] != '?' && url[end] != '#') ++end;
    *host = url + 8; *host_bytes = slash - 8; *path = url + slash; *path_bytes = end - slash;
    return true;
}
// Path segment i (0-based) of /a/b/c; false when absent.
bool segment(const char* path, u32 bytes, u32 index, const char** out, u32* out_bytes) noexcept {
    u32 cursor = 0;
    for (u32 i = 0;; ++i) {
        if (cursor >= bytes || path[cursor] != '/') return false;
        u32 end = cursor + 1;
        while (end < bytes && path[end] != '/') ++end;
        if (i == index) { *out = path + cursor + 1; *out_bytes = end - cursor - 1; return *out_bytes != 0; }
        cursor = end;
    }
}
u32 segment_count(const char* path, u32 bytes) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < bytes; ++i) count += path[i] == '/';
    return count;
}
MediaUrlResult publish_provider(CdnHost host, const u8* target, u32 bytes, Slice<u8> output) noexcept {
    if (!catalog_provider_target_valid(host, {target, bytes}))
        return media_url_failure(MediaUrlError::invalid_target);
    if (bytes > output.count)
        return media_url_failure(MediaUrlError::output_capacity);
    copy_bytes(output.data, target, bytes);
    return {bytes, host, MediaUrlError::none};
}
bool tenor_host(const char* host, u32 bytes) noexcept {
    return ascii_equal(host, bytes, "media.tenor.com") || ascii_equal(host, bytes, "media.tenor.co") ||
           ascii_equal(host, bytes, "c.tenor.com") || ascii_equal(host, bytes, "media1.tenor.com");
}
bool giphy_host(const char* host, u32 bytes) noexcept {
    return ascii_equal(host, bytes, "media.giphy.com") || ascii_equal(host, bytes, "i.giphy.com") ||
           (bytes == 16 && ascii_equal(host, 5, "media") && host[5] >= '0' && host[5] <= '4' &&
            ascii_equal(host + 6, 10, ".giphy.com"));
}
// Tenor/Giphy media in any rendition (gif, mp4, webm, webp, png) maps to the
// provider's tiny animated WebP: Tenor ids end in AAA + a two-letter format
// code (A1 is tinywebp); Giphy serves /media/<id>/200w.webp for every id.
MediaUrlResult provider_preview(const char* host, u32 host_bytes, const char* path, u32 path_bytes,
                                Slice<u8> output) noexcept {
    u8 target[256];
    const char* part = nullptr;
    u32 part_bytes = 0;
    if (tenor_host(host, host_bytes)) {
        // media1 prefixes /m/; the id segment is followed by the filename.
        const u32 first = path_bytes > 3 && bytes_equal(path, "/m/", 3) ? 1u : 0u;
        const char* name = nullptr;
        u32 name_bytes = 0;
        if (segment_count(path, path_bytes) != first + 2 || !segment(path, path_bytes, first, &part, &part_bytes) ||
            !segment(path, path_bytes, first + 1, &name, &name_bytes) || part_bytes != 16 ||
            !bytes_equal(part + 11, "AAA", 3))
            return media_url_failure(MediaUrlError::invalid_target);
        u32 stem = 0;
        while (stem < name_bytes && name[stem] != '.') ++stem;
        if (!stem || stem > 128) return media_url_failure(MediaUrlError::invalid_target);
        target[0] = '/';
        copy_bytes(target + 1, part, 11);
        copy_bytes(target + 12, "AAAA1/", 6);
        copy_bytes(target + 18, name, stem);
        copy_bytes(target + 18 + stem, ".webp", 5);
        return publish_provider(CdnHost::catalog_tenor, target, 23 + stem, output);
    }
    if (!giphy_host(host, host_bytes)) return media_url_failure(MediaUrlError::invalid_host);
    const u32 count = segment_count(path, path_bytes);
    const char* first = nullptr;
    u32 first_bytes = 0;
    if (!segment(path, path_bytes, 0, &first, &first_bytes)) return media_url_failure(MediaUrlError::invalid_target);
    if (count == 1) {
        // i.giphy.com/<id>.<ext>
        part = first;
        part_bytes = 0;
        while (part_bytes < first_bytes && first[part_bytes] != '.') ++part_bytes;
    } else if (first_bytes == 5 && bytes_equal(first, "media", 5) && count >= 3) {
        // /media/<id>/<file> or /media/v1.<token>/<id>/<file>
        if (!segment(path, path_bytes, count - 2, &part, &part_bytes))
            return media_url_failure(MediaUrlError::invalid_target);
    } else {
        return media_url_failure(MediaUrlError::invalid_target);
    }
    if (!part_bytes || part_bytes > 64) return media_url_failure(MediaUrlError::invalid_target);
    copy_bytes(target, "/media/", 7);
    copy_bytes(target + 7, part, part_bytes);
    copy_bytes(target + 7 + part_bytes, "/200w.webp", 10);
    return publish_provider(CdnHost::catalog_giphy, target, 17 + part_bytes, output);
}
bool path_ends_with(const char* path, u32 bytes, const char* suffix) noexcept {
    const u32 length = literal_length(suffix);
    if (bytes < length) return false;
    for (u32 i = 0; i < length; ++i)
        if (ascii_lower(static_cast<u8>(path[bytes - length + i])) != static_cast<u8>(suffix[i])) return false;
    return true;
}
bool video_path(const char* path, u32 bytes) noexcept {
    return path_ends_with(path, bytes, ".mp4") || path_ends_with(path, bytes, ".webm") ||
           path_ends_with(path, bytes, ".mov") || path_ends_with(path, bytes, ".m4v");
}
// Fits width x height (0: a square) inside the 160 px catalog preview box.
void preview_box(u32 width, u32 height, u32* w, u32* h) noexcept {
    u64 x = width ? width : 160, y = height ? height : 160;
    const u64 longest = x > y ? x : y;
    if (longest > 160) {
        x = maximum<u64>(1, x * 160 / longest);
        y = maximum<u64>(1, y * 160 / longest);
    }
    *w = static_cast<u32>(x);
    *h = static_cast<u32>(y);
}
// Media-proxy path for an external resource:
//   /external/<signature>[/<%3F-encoded query>]/<https|http>/<host><path>[?query]
// The proxy refuses an unsigned or altered path, so an external host is
// reachable only through a signed URL the service issued. Shape bounds: no dot
// or empty path segments, no encoded dot, slash or backslash, non-empty host.
struct ExternalProxyTarget {
    const char* host;
    const char* path;
    u32 host_bytes;
    u32 path_bytes;
    u32 path_end; // Offset of the stored query ('?') or the target's end.
};
bool external_proxy_target(ConstSlice<u8> target, ExternalProxyTarget* out) noexcept {
    constexpr u32 prefix = sizeof("/external/") - 1;
    if (!target.data || !bytes_begin_with(target, target.count, "/external/"))
        return false;
    const auto text = reinterpret_cast<const char*>(target.data);
    const auto alnum = [](u8 c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    u32 cursor = prefix;
    while (cursor < target.count && target[cursor] != '/') {
        const u8 c = target[cursor++];
        if (!alnum(c) && c != '_' && c != '-')
            return false;
    }
    if (cursor == prefix || cursor - prefix > 128 || cursor == target.count)
        return false;
    if (target.count - cursor > 4 && target[cursor + 1] == '%' && target[cursor + 2] == '3' &&
        (target[cursor + 3] == 'F' || target[cursor + 3] == 'f')) {
        ++cursor;
        while (cursor < target.count && target[cursor] != '/' && target[cursor] != '?')
            ++cursor;
        if (cursor == target.count || target[cursor] != '/')
            return false;
    }
    const u32 remaining = target.count - cursor;
    const u32 scheme = remaining > 7 && bytes_equal(text + cursor, "/https/", 7)  ? 7
                       : remaining > 6 && bytes_equal(text + cursor, "/http/", 6) ? 6
                                                                                  : 0;
    if (!scheme)
        return false;
    cursor += scheme;
    const u32 host = cursor;
    while (cursor < target.count && target[cursor] != '/' && target[cursor] != '?') {
        const u8 c = target[cursor++];
        if (!alnum(c) && c != '.' && c != '-' && c != ':')
            return false;
    }
    if (cursor == host || cursor - host > 253 || cursor == target.count || target[cursor] != '/')
        return false;
    const u32 path = cursor;
    while (cursor < target.count && target[cursor] != '?')
        ++cursor;
    for (u32 begin = path; begin < cursor;) {
        u32 end = begin + 1;
        while (end < cursor && target[end] != '/')
            ++end;
        const u32 length = end - begin - 1;
        if (!length || (length == 1 && target[begin + 1] == '.') ||
            (length == 2 && target[begin + 1] == '.' && target[begin + 2] == '.'))
            return false;
        begin = end;
    }
    for (u32 i = path; i + 2 < cursor; ++i) {
        if (target[i] != '%')
            continue;
        const u8 high = target[i + 1], low = ascii_lower(target[i + 2]);
        if ((high == '2' && (low == 'e' || low == 'f')) || (high == '5' && low == 'c'))
            return false;
    }
    *out = {text + host, text + path, path - host, cursor - path, cursor};
    return true;
}
// The proxy's bounded animated WebP rendition of an external image: its signed
// path, with any stored query replaced by the client's own exact query.
MediaUrlResult external_proxy_webp(ConstSlice<u8> wrapped, u32 path_end, CdnHost host, u32 width,
                                   u32 height, Slice<u8> output) noexcept {
    u32 w = 0, h = 0;
    preview_box(width, height, &w, &h);
    u8 rewritten[kCdnMaximumTargetBytes];
    ProtocolWriter writer{{rewritten, sizeof(rewritten)}, 0};
    if (!protocol_writer_append_bytes(&writer, wrapped.data, path_end) ||
        !protocol_writer_append_literal(&writer, "?format=webp&animated=true&width=") ||
        !protocol_writer_append_u64(&writer, w) || !protocol_writer_append_literal(&writer, "&height=") ||
        !protocol_writer_append_u64(&writer, h))
        return media_url_failure(MediaUrlError::invalid_target);
    if (writer.used > output.count)
        return media_url_failure(MediaUrlError::output_capacity);
    copy_bytes(output.data, rewritten, writer.used);
    return {writer.used, host, MediaUrlError::none};
}
// A VIDEO favourite played as a muted looping tile: a Klipy MP4 asset (stored
// when Klipy had no gif_src), a Discord attachment MP4, or an MP4 behind the
// external media proxy. Tenor and Giphy videos use their providers' tiny
// animated WebP instead (cheaper).
MediaUrlResult video_favourite(Utf8View url, Slice<u8> output) noexcept {
    const char* host = nullptr;
    const char* path = nullptr;
    u32 host_bytes = 0, path_bytes = 0;
    if (!split_https(url.data, url.bytes, &host, &host_bytes, &path, &path_bytes))
        return media_url_failure(MediaUrlError::invalid_scheme);
    if (ascii_equal(host, host_bytes, kCatalogKlipyHost) || ascii_equal(host, host_bytes, kCatalogKlipyStatic2Host)) {
        const ConstSlice<u8> target{reinterpret_cast<const u8*>(path),
                                    static_cast<u32>(url.data + url.bytes - path)};
        if (!klipy_asset_valid(target, ".mp4", 4))
            return media_url_failure(MediaUrlError::invalid_target);
        if (target.count > output.count)
            return media_url_failure(MediaUrlError::output_capacity);
        copy_bytes(output.data, target.data, target.count);
        return {target.count,
                ascii_equal(host, host_bytes, kCatalogKlipyHost) ? CdnHost::catalog_klipy
                                                                 : CdnHost::catalog_klipy2,
                MediaUrlError::none};
    }
    u8 source[kCdnMaximumTargetBytes];
    const auto parsed = parse_media_url(url, {source, sizeof(source)});
    if (!parsed.ok())
        return parsed;
    ConstSlice<u8> target{source, parsed.target_bytes};
    ExternalProxyTarget external{};
    if (parsed.host != CdnHost::cdn && external_proxy_target(target, &external)) {
        if (tenor_host(external.host, external.host_bytes) || giphy_host(external.host, external.host_bytes) ||
            !path_ends_with(external.path, external.path_bytes, ".mp4"))
            return media_url_failure(MediaUrlError::invalid_target);
        target.count = external.path_end;
    } else {
        u32 query = 0;
        while (query < target.count && target[query] != '?')
            ++query;
        if ((parsed.host != CdnHost::cdn && parsed.host != CdnHost::media) ||
            !bytes_begin_with(target, query, "/attachments/") ||
            !path_ends_with(reinterpret_cast<const char*>(source), query, ".mp4"))
            return media_url_failure(MediaUrlError::invalid_target);
    }
    if (target.count > output.count)
        return media_url_failure(MediaUrlError::output_capacity);
    copy_bytes(output.data, target.data, target.count);
    return {target.count, parsed.host, MediaUrlError::none};
}
// A Klipy favourite's bare asset path ending in suffix, fetched from its origin.
MediaUrlResult klipy_favourite(Utf8View url, const KlipyOrigin& klipy, const char* suffix, u32 suffix_bytes,
                               Slice<u8> output) noexcept {
    const ConstSlice<u8> target{reinterpret_cast<const u8*>(url.data + klipy.bytes), url.bytes - klipy.bytes};
    const u32 path = klipy_favourite_path(target, suffix, suffix_bytes);
    if (!path)
        return media_url_failure(MediaUrlError::invalid_target);
    if (path > output.count)
        return media_url_failure(MediaUrlError::output_capacity);
    copy_bytes(output.data, target.data, path);
    return {path, klipy.host, MediaUrlError::none};
}
} // namespace

bool catalog_provider_target_valid(CdnHost host, ConstSlice<u8> target) noexcept {
    switch (host) {
    case CdnHost::catalog_klipy:
    case CdnHost::catalog_klipy2:
        return klipy_asset_valid(target, ".webp", 5) || klipy_asset_valid(target, ".gif", 4) ||
               klipy_asset_valid(target, ".mp4", 4);
    case CdnHost::catalog_tenor: return tenor_target_valid(target);
    case CdnHost::catalog_giphy: return giphy_target_valid(target);
    default: return false;
    }
}

MediaUrlResult parse_catalog_preview_url(Utf8View url, Slice<u8> target_output,
                                         MediaAttachmentFormat format, u32 width,
                                         u32 height) noexcept {
    const KlipyOrigin klipy = klipy_origin(url);
    if (!url.data || !url.bytes || (!target_output.data && target_output.count))
        return media_url_failure(MediaUrlError::invalid_input);
    if (format == MediaAttachmentFormat::lottie) {
        auto parsed = parse_media_url(url, target_output);
        if (!parsed.ok())
            return parsed;
        if (parsed.host != CdnHost::cdn ||
            !sticker_json_target_valid({target_output.data, parsed.target_bytes}))
            return media_url_failure(MediaUrlError::invalid_target);
        return parsed;
    }
    if (format == MediaAttachmentFormat::gif && klipy.bytes) {
        // A Klipy GIF favourite is stored as its gif_src (.gif), with
        // ?format=webp&animated=true appended when it came from a proxied embed.
        return klipy_favourite(url, klipy, ".gif", 4, target_output);
    }
    if (format == MediaAttachmentFormat::png || format == MediaAttachmentFormat::gif) {
        auto parsed = parse_media_url(url, target_output);
        if (!parsed.ok())
            return parsed;
        const ConstSlice<u8> target{target_output.data, parsed.target_bytes};
        if ((parsed.host != CdnHost::cdn && parsed.host != CdnHost::media) ||
            !bytes_begin_with(target, target.count, "/stickers/") || target.count <= 13)
            return media_url_failure(MediaUrlError::invalid_target);
        const char* suffix = format == MediaAttachmentFormat::png ? ".png" : ".gif";
        u64 sticker = 0;
        if (!ascii_equal(reinterpret_cast<const char*>(target.data + target.count - 4), 4, suffix) ||
            !parse_decimal(reinterpret_cast<const char*>(target.data + 10), target.count - 14, &sticker) ||
            !sticker)
            return media_url_failure(MediaUrlError::invalid_target);
        return parsed;
    }
    if (format == MediaAttachmentFormat::mp4)
        return video_favourite(url, target_output);
    if (format != MediaAttachmentFormat::webp)
        return media_url_failure(MediaUrlError::invalid_input);
    constexpr char proxy_1[] = "https://images-ext-1.discordapp.net/";
    constexpr char proxy_2[] = "https://images-ext-2.discordapp.net/";
    constexpr char media_external[] = "https://media.discordapp.net/external/";
    const bool images_ext = url.bytes >= sizeof(proxy_1) - 1 && (ascii_equal(url.data, sizeof(proxy_1) - 1, proxy_1) ||
                                                                 ascii_equal(url.data, sizeof(proxy_2) - 1, proxy_2));
    if (images_ext ||
        (url.bytes >= sizeof(media_external) - 1 && ascii_equal(url.data, sizeof(media_external) - 1, media_external))) {
        const auto parsed = parse_media_url(url, target_output);
        if (!parsed.ok())
            return parsed;
        u8 wrapped_bytes[kCdnMaximumTargetBytes];
        copy_bytes(wrapped_bytes, target_output.data, parsed.target_bytes);
        const ConstSlice<u8> wrapped{wrapped_bytes, parsed.target_bytes};
        // Discord's category tiles: the exact animated WebP query it issued.
        if (images_ext && catalog_proxy_target_valid(wrapped))
            return parsed;
        // Any other external favourite (proxied embed media such as a Klipy gifv
        // thumbnail) keeps its signed proxy path; Tenor and Giphy media load
        // their providers' tiny rendition instead.
        ExternalProxyTarget external{};
        if (!external_proxy_target(wrapped, &external))
            return media_url_failure(MediaUrlError::invalid_target);
        if (tenor_host(external.host, external.host_bytes) || giphy_host(external.host, external.host_bytes)) {
            const auto provider =
                provider_preview(external.host, external.host_bytes, external.path, external.path_bytes, target_output);
            if (provider.ok() || provider.error == MediaUrlError::output_capacity)
                return provider;
        }
        // Video favourites play through the mp4 format instead.
        if (video_path(external.path, external.path_bytes))
            return media_url_failure(MediaUrlError::invalid_target);
        return external_proxy_webp(wrapped, external.path_end, parsed.host, width, height, target_output);
    }
    {
        const char* host = nullptr; const char* path = nullptr;
        u32 host_bytes = 0, path_bytes = 0;
        if (split_https(url.data, url.bytes, &host, &host_bytes, &path, &path_bytes) && !klipy.bytes) {
            if (ascii_equal(host, host_bytes, kCdnHost) || ascii_equal(host, host_bytes, kMediaProxyHost)) {
                // An attachment GIF/WebP favourited from a message: fetched as the
                // media proxy's animated WebP.
                u8 source[kCdnMaximumTargetBytes];
                const auto parsed = parse_media_url(url, {source, sizeof(source)});
                u32 query = 0;
                while (parsed.ok() && query < parsed.target_bytes && source[query] != '?')
                    ++query;
                // A video attachment plays through the mp4 format instead.
                if (!parsed.ok() || !bytes_begin_with({source, parsed.target_bytes}, parsed.target_bytes, "/attachments/") ||
                    video_path(reinterpret_cast<const char*>(source), query))
                    return media_url_failure(MediaUrlError::invalid_target);
                u32 w = 0, h = 0;
                preview_box(width, height, &w, &h);
                const auto written = write_media_proxy_target({source, parsed.target_bytes}, parsed.host, w, h,
                                                              CdnMediaFormat::webp, target_output, true);
                if (!written.ok())
                    return media_url_failure(written.error == CdnTargetError::output_capacity
                                                 ? MediaUrlError::output_capacity
                                                 : MediaUrlError::invalid_target);
                return {written.bytes, written.host, MediaUrlError::none};
            }
            for (u32 i = path_bytes; i < url.bytes; ++i)
                if (url.data[i] == '#') return media_url_failure(MediaUrlError::invalid_target);
            // Tenor/Giphy direct. Any other raw host is refused: only a URL
            // Discord's proxy signed may reach an arbitrary origin.
            return provider_preview(host, host_bytes, path, path_bytes, target_output);
        }
    }
    if (!klipy.bytes)
        return media_url_failure(MediaUrlError::invalid_host);
    // A direct Klipy animated WebP, or an IMAGE favourite of a Klipy gifv embed
    // (its thumbnail, stored with ?animated=true), fetched as the bare asset
    // path. MP4 VIDEO favourites use the mp4 format.
    return klipy_favourite(url, klipy, ".webp", 5, target_output);
}

MediaAttachmentFormat catalog_favorite_format(Utf8View src) noexcept {
    if (!src.data || !src.bytes)
        return MediaAttachmentFormat::webp;
    const KlipyOrigin klipy = klipy_origin(src);
    if (klipy.bytes &&
        klipy_favourite_path({reinterpret_cast<const u8*>(src.data + klipy.bytes), src.bytes - klipy.bytes}, ".gif",
                             4))
        return MediaAttachmentFormat::gif;
    // Pickers ask per visible tile and frame: only an .mp4 path pays for the
    // full admission parse.
    u32 path_end = 0;
    while (path_end < src.bytes && src.data[path_end] != '?' && src.data[path_end] != '#')
        ++path_end;
    if (!path_ends_with(src.data, path_end, ".mp4"))
        return MediaAttachmentFormat::webp;
    u8 scratch[kCdnMaximumTargetBytes];
    return video_favourite(src, {scratch, sizeof(scratch)}).ok() ? MediaAttachmentFormat::mp4
                                                                 : MediaAttachmentFormat::webp;
}

u32 media_url_log_fields(Utf8View url, Slice<char> output) noexcept {
    u32 used = 0;
    const auto put = [&](const char* text) noexcept {
        while (*text && used < output.count)
            output[used++] = *text++;
    };
    // Only host-name bytes are copied, so nothing else of a URL can leak.
    const auto host_field = [&](const char* key, const char* host, u32 bytes) noexcept {
        put(key);
        for (u32 i = 0; i < bytes && i < 253 && used < output.count; ++i) {
            const char c = host[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')
                output[used++] = c;
        }
    };
    if (!output.data || !url.data)
        return 0;
    const char* host = nullptr;
    const char* path = nullptr;
    u32 host_bytes = 0, path_bytes = 0;
    if (!split_https(url.data, url.bytes, &host, &host_bytes, &path, &path_bytes)) {
        put(" host=none");
        return used;
    }
    host_field(" host=", host, host_bytes);
    // The last path segment's extension: one to five letters or digits.
    u32 segment = path_bytes, dot = path_bytes;
    while (segment && path[segment - 1] != '/')
        --segment;
    while (dot > segment && path[dot - 1] != '.')
        --dot;
    bool extension = dot > segment && path_bytes - dot >= 1 && path_bytes - dot <= 5;
    for (u32 i = dot; extension && i < path_bytes; ++i) {
        const char c = path[i];
        extension = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    }
    put(" ext=");
    if (!extension)
        put("none");
    for (u32 i = dot; extension && i < path_bytes && used < output.count; ++i)
        output[used++] = path[i];
    ExternalProxyTarget external{};
    if (external_proxy_target({reinterpret_cast<const u8*>(path), path_bytes}, &external))
        host_field(" via=", external.host, external.host_bytes);
    return used;
}

CdnTargetResult write_media_proxy_target(ConstSlice<u8> source_target,
                                         CdnHost source_host, u32 target_width,
                                         u32 target_height, CdnMediaFormat format,
                                         Slice<u8> output, bool animated) noexcept {
    if (!source_target.data || source_target.count == 0 || source_target[0] != '/' ||
        (source_host != CdnHost::cdn && source_host != CdnHost::media &&
         source_host != CdnHost::images_ext_1 && source_host != CdnHost::images_ext_2) ||
        target_width == 0 || target_height == 0 || target_width > 4'096 || target_height > 4'096 ||
        (!output.data && output.count != 0)) {
        return failure(CdnTargetError::invalid_input);
    }
    u32 query = 0;
    while (query < source_target.count && source_target[query] != '?') {
        ++query;
    }
    const bool size_endpoint = bytes_begin_with(source_target, query, "/avatars/") ||
                               bytes_begin_with(source_target, query, "/icons/") ||
                               bytes_begin_with(source_target, query, "/embed/avatars/");
    if (size_endpoint && (target_width != target_height || !image_size_valid(target_width))) {
        return failure(CdnTargetError::invalid_input);
    }
    ProtocolWriter writer{output, 0};
    if (!protocol_writer_append_bytes(&writer, ConstSlice<u8>{source_target.data, query})) {
        return failure(CdnTargetError::output_capacity);
    }
    bool has_query = false;
    u32 cursor = query < source_target.count ? query + 1 : query;
    while (cursor < source_target.count) {
        u32 end = cursor;
        while (end < source_target.count && source_target[end] != '&') {
            ++end;
        }
        u32 equals = cursor;
        while (equals < end && source_target[equals] != '=') {
            ++equals;
        }
        bool proxy_hint = equals == cursor && equals + 1 == end;
        if (equals < end && equals != cursor) {
            const Utf8View name{reinterpret_cast<const char*>(source_target.data + cursor),
                                equals - cursor};
            proxy_hint = ascii_equal(name.data, name.bytes, "width") ||
                         ascii_equal(name.data, name.bytes, "height") ||
                         ascii_equal(name.data, name.bytes, "size") ||
                         ascii_equal(name.data, name.bytes, "format") ||
                         (animated && ascii_equal(name.data, name.bytes, "animated")) ||
                         ascii_equal(name.data, name.bytes, "quality");
        }
        if (!proxy_hint && end != cursor) {
            if (!protocol_writer_append_byte(&writer, has_query ? '&' : '?') ||
                !protocol_writer_append_bytes(&writer,
                                              ConstSlice<u8>{source_target.data + cursor, end - cursor})) {
                return failure(CdnTargetError::output_capacity);
            }
            has_query = true;
        }
        cursor = end < source_target.count ? end + 1 : end;
    }
    if (!protocol_writer_append_byte(&writer, has_query ? '&' : '?') ||
        !protocol_writer_append_c_string(&writer, size_endpoint ? "size=" : "width=") ||
        !protocol_writer_append_u64(&writer, target_width) ||
        (!size_endpoint && (!protocol_writer_append_literal(&writer, "&height=") ||
                            !protocol_writer_append_u64(&writer, target_height)))) {
        return failure(CdnTargetError::output_capacity);
    }
    if (!size_endpoint && format != CdnMediaFormat::source &&
        (!protocol_writer_append_literal(&writer, "&format=") ||
         !(format == CdnMediaFormat::png    ? protocol_writer_append_literal(&writer, "png")
           : format == CdnMediaFormat::webp ? protocol_writer_append_literal(&writer, "webp")
                                                : protocol_writer_append_literal(&writer, "jpeg")))) {
        return failure(CdnTargetError::output_capacity);
    }
    if (animated && !protocol_writer_append_literal(&writer, "&animated=true")) {
        return failure(CdnTargetError::output_capacity);
    }
    return CdnTargetResult{writer.used, size_endpoint ? CdnHost::cdn : CdnHost::media,
                           CdnTargetError::none};
}

CdnTargetResult write_custom_emoji_cdn_target(u64 emoji_id, bool animated, u32 size,
                                              Slice<u8> output) noexcept {
    if (emoji_id == 0 || !image_size_valid(size) || (!output.data && output.count != 0)) {
        return failure(CdnTargetError::invalid_input);
    }
    u8 target[96];
    ProtocolWriter writer{Slice<u8>{target, sizeof(target)}, 0};
    const bool written = protocol_writer_append_literal(&writer, "/emojis/") &&
                         protocol_writer_append_u64(&writer, emoji_id) &&
                         protocol_writer_append_literal(&writer, ".webp?size=") &&
                         protocol_writer_append_u64(&writer, size) &&
                         (!animated || protocol_writer_append_literal(&writer, "&animated=true"));
    return written ? publish_target(ConstSlice<u8>{target, writer.used}, CdnHost::cdn, output)
                   : failure(CdnTargetError::output_capacity);
}

u32 cdn_guild_icon_variant(Utf8View icon_hash) noexcept {
    if (!guild_icon_hash_valid(icon_hash)) {
        return 0;
    }
    u32 value = 2'166'136'261u;
    for (u32 index = 0; index < icon_hash.bytes; ++index) {
        value ^= static_cast<u8>(icon_hash.data[index]);
        value *= 16'777'619u;
    }
    return value == 0 ? 1u : value;
}

namespace {
// "<prefix><id>/<hash>.webp?size=<size>", animated when allowed and the hash is.
template <usize N>
CdnTargetResult write_hashed_asset_target(const char (&prefix)[N], u64 id, Utf8View hash, u32 size,
                                          bool animate, Slice<u8> output) noexcept {
    if (id == 0 || !guild_icon_hash_valid(hash) || !image_size_valid(size) ||
        (!output.data && output.count != 0)) {
        return failure(CdnTargetError::invalid_input);
    }
    u8 target[128];
    ProtocolWriter writer{Slice<u8>{target, sizeof(target)}, 0};
    const bool animated = animate && asset_hash_animated(hash);
    const bool written =
        protocol_writer_append_literal(&writer, prefix) &&
        protocol_writer_append_u64(&writer, id) && protocol_writer_append_byte(&writer, '/') &&
        protocol_writer_append_bytes(&writer, ConstSlice<u8>{reinterpret_cast<const u8*>(hash.data), hash.bytes}) &&
        protocol_writer_append_literal(&writer, ".webp?size=") &&
        protocol_writer_append_u64(&writer, size) &&
        (!animated || protocol_writer_append_literal(&writer, "&animated=true"));
    return written ? publish_target(ConstSlice<u8>{target, writer.used}, CdnHost::cdn, output)
                   : failure(CdnTargetError::output_capacity);
}
} // namespace

CdnTargetResult write_guild_icon_cdn_target(u64 guild_id, Utf8View icon_hash, u32 size,
                                            Slice<u8> output, bool animate) noexcept {
    return write_hashed_asset_target("/icons/", guild_id, icon_hash, size, animate, output);
}

CdnTargetResult write_user_avatar_cdn_target(u64 user_id, Utf8View avatar_hash, u32 size,
                                             Slice<u8> output) noexcept {
    return write_hashed_asset_target("/avatars/", user_id, avatar_hash, size, true, output);
}

CdnTargetResult write_channel_icon_cdn_target(u64 channel_id, Utf8View icon_hash, u32 size,
                                              Slice<u8> output) noexcept {
    return write_hashed_asset_target("/channel-icons/", channel_id, icon_hash, size, true, output);
}

CdnTargetResult write_sticker_cdn_target(u64 sticker_id, u32 format_type,
                                         Slice<u8> output) noexcept {
    if (sticker_id == 0 || (!output.data && output.count != 0)) {
        return failure(CdnTargetError::invalid_input);
    }
    const char* extension = nullptr;
    CdnHost host = CdnHost::cdn;
    switch (format_type) {
    case 1:
    case 2:
        extension = ".png";
        break;
    case 3:
        extension = ".json";
        break;
    case 4:
        extension = ".gif";
        host = CdnHost::media;
        break;
    default:
        return failure(CdnTargetError::unsupported_format);
    }
    u8 target[64];
    ProtocolWriter writer{Slice<u8>{target, sizeof(target)}, 0};
    const bool written = protocol_writer_append_literal(&writer, "/stickers/") &&
                         protocol_writer_append_u64(&writer, sticker_id) &&
                         protocol_writer_append_c_string(&writer, extension);
    return written ? publish_target(ConstSlice<u8>{target, writer.used}, host, output)
                   : failure(CdnTargetError::output_capacity);
}

Utf8View cdn_host_name(CdnHost host) noexcept {
    switch (host) {
    case CdnHost::cdn:
        return utf8_literal("cdn.discordapp.com");
    case CdnHost::media:
        return utf8_literal("media.discordapp.net");
    case CdnHost::images_ext_1:
        return utf8_literal("images-ext-1.discordapp.net");
    case CdnHost::images_ext_2:
        return utf8_literal("images-ext-2.discordapp.net");
    case CdnHost::flag_assets:
        return utf8_literal("raw.githubusercontent.com");
    case CdnHost::catalog_klipy:
        return utf8_literal("static.klipy.com");
    case CdnHost::catalog_klipy2:
        return utf8_literal("static2.klipy.com");
    case CdnHost::catalog_tenor:
        return utf8_literal("media.tenor.com");
    case CdnHost::catalog_giphy:
        return utf8_literal("media.giphy.com");
    case CdnHost::activity_art:
        return utf8_literal(kActivityArtHost);
    default:
        return Utf8View{};
    }
}

} // namespace maicord
