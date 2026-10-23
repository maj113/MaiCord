#include "maicord/protocol/cdn_http.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/protocol/http1.hpp"

namespace maicord {
bool catalog_provider_request_valid(CdnHost host, CdnImageType type,
                                    ConstSlice<u8> target) noexcept {
    if (!catalog_provider_target_valid(host, target) || target.count < 5)
        return false;
    const auto suffix = [&](const char* text, u32 bytes) noexcept {
        return bytes_equal(target.data + target.count - bytes, text, bytes);
    };
    // The response type must agree with the asset's own extension; only Klipy
    // serves GIF and MP4 assets directly.
    return type == CdnImageType::webp  ? suffix(".webp", 5)
           : type == CdnImageType::gif ? cdn_klipy_host(host) && suffix(".gif", 4)
           : type == CdnImageType::mp4 ? cdn_klipy_host(host) && suffix(".mp4", 4)
                                              : false;
}
namespace {

struct ResponseFields {
    ConstSlice<u8> content_type;
    ConstSlice<u8> content_encoding;
    ConstSlice<u8> location;
    ConstSlice<u8> etag;
    ConstSlice<u8> content_range;
    u64 max_age;
    u64 age;
    CdnHttpError error;
    bool has_content_type;
    bool has_content_encoding;
    bool has_location;
    bool has_etag;
    bool has_content_range;
    bool has_max_age;
    bool has_age;
    bool no_store;
    bool no_cache;
};

#include "cdn_validation.inc"

bool slice_equal_case(ConstSlice<u8> left, ConstSlice<u8> right) noexcept {
    if (left.count != right.count) {
        return false;
    }
    for (u32 index = 0; index < left.count; ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

bool parse_decimal(ConstSlice<u8> value, u64* result) noexcept {
    if (value.count == 0) {
        return false;
    }
    u64 parsed = 0;
    for (u32 index = 0; index < value.count; ++index) {
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
        const u64 digit = value[index] - '0';
        if (parsed > (~0ull - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    *result = parsed;
    return true;
}

bool parse_delta_seconds(ConstSlice<u8> value, u64* result) noexcept {
    value = http_trim_ows(value);
    if (value.count >= 2 && value[0] == '"' && value[value.count - 1] == '"') {
        value = ConstSlice<u8>{value.data + 1, value.count - 2};
    }
    return parse_decimal(value, result);
}

bool name_equal(ConstSlice<u8> name, const char* literal) noexcept {
    return http_ascii_equal_case_insensitive(name, literal);
}

bool parse_cache_control(ResponseFields* fields, ConstSlice<u8> value) noexcept {
    u32 begin = 0;
    while (begin < value.count) {
        u32 end = begin;
        bool quoted = false;
        bool escaped = false;
        while (end < value.count) {
            const u8 current = value[end];
            if (escaped) {
                escaped = false;
            } else if (quoted && current == '\\') {
                escaped = true;
            } else if (current == '"') {
                quoted = !quoted;
            } else if (!quoted && current == ',') {
                break;
            }
            ++end;
        }
        if (quoted || escaped) {
            return false;
        }
        ConstSlice<u8> directive = http_trim_ows(ConstSlice<u8>{value.data + begin, end - begin});
        if (directive.count == 0) {
            return false;
        }
        u32 equals = directive.count;
        for (u32 index = 0; index < directive.count; ++index) {
            if (directive[index] == '=') {
                equals = index;
                break;
            }
        }
        const ConstSlice<u8> name = http_trim_ows(ConstSlice<u8>{directive.data, equals});
        const bool has_value = equals < directive.count;
        const ConstSlice<u8> directive_value =
            has_value ? ConstSlice<u8>{directive.data + equals + 1, directive.count - equals - 1}
                      : ConstSlice<u8>{};
        if (name_equal(name, "no-store")) {
            fields->no_store = true;
        } else if (name_equal(name, "no-cache")) {
            fields->no_cache = true;
        } else if (name_equal(name, "max-age")) {
            u64 seconds = 0;
            if (!has_value || fields->has_max_age || !parse_delta_seconds(directive_value, &seconds)) {
                return false;
            }
            fields->has_max_age = true;
            fields->max_age = seconds;
        }
        if (end < value.count && end + 1 == value.count) {
            return false;
        }
        begin = end + (end < value.count ? 1u : 0u);
    }
    return true;
}

bool response_field_visitor(void* context, HttpField field) noexcept {
    ResponseFields* fields = static_cast<ResponseFields*>(context);
    if (name_equal(field.name, "content-type")) {
        if (fields->has_content_type) {
            fields->error = CdnHttpError::duplicate_field;
            return false;
        }
        fields->has_content_type = true;
        fields->content_type = field.value;
    } else if (name_equal(field.name, "content-encoding")) {
        if (fields->has_content_encoding) {
            fields->error = CdnHttpError::duplicate_field;
            return false;
        }
        fields->has_content_encoding = true;
        fields->content_encoding = field.value;
    } else if (name_equal(field.name, "location")) {
        if (fields->has_location) {
            fields->error = CdnHttpError::duplicate_field;
            return false;
        }
        fields->has_location = true;
        fields->location = field.value;
    } else if (name_equal(field.name, "etag")) {
        if (fields->has_etag || field.value.count == 0 || !valid_etag(field.value)) {
            fields->error =
                fields->has_etag ? CdnHttpError::duplicate_field : CdnHttpError::invalid_etag;
            return false;
        }
        fields->has_etag = true;
        fields->etag = field.value;
    } else if (name_equal(field.name, "content-range")) {
        if (fields->has_content_range) {
            fields->error = CdnHttpError::duplicate_field;
            return false;
        }
        fields->has_content_range = true;
        fields->content_range = field.value;
    } else if (name_equal(field.name, "age")) {
        if (fields->has_age || !parse_decimal(field.value, &fields->age)) {
            fields->error =
                fields->has_age ? CdnHttpError::duplicate_field : CdnHttpError::invalid_age;
            return false;
        }
        fields->has_age = true;
    } else if (name_equal(field.name, "cache-control")) {
        if (!parse_cache_control(fields, field.value)) {
            fields->error = CdnHttpError::invalid_cache_control;
            return false;
        }
    }
    return true;
}

CdnResponseResult response_failure(CdnHttpError error, u16 status = 0) noexcept {
    CdnResponseResult result{};
    result.status_code = status;
    result.error = error;
    return result;
}

// "bytes first-last/length" with a known length; no unsatisfied or multipart form.
bool parse_content_range(ConstSlice<u8> value, u64* first, u64* last, u64* length) noexcept {
    value = http_trim_ows(value);
    constexpr char unit[] = "bytes ";
    constexpr u32 unit_bytes = sizeof(unit) - 1;
    if (value.count <= unit_bytes ||
        !slice_equal_case({value.data, unit_bytes}, {reinterpret_cast<const u8*>(unit), unit_bytes}))
        return false;
    u32 dash = unit_bytes;
    while (dash < value.count && value[dash] != '-')
        ++dash;
    u32 slash = dash;
    while (slash < value.count && value[slash] != '/')
        ++slash;
    if (dash >= value.count || slash >= value.count)
        return false;
    return parse_decimal({value.data + unit_bytes, dash - unit_bytes}, first) &&
           parse_decimal({value.data + dash + 1, slash - dash - 1}, last) &&
           parse_decimal({value.data + slash + 1, value.count - slash - 1}, length) && *first <= *last &&
           *last < *length;
}

CdnResponseResult validate_response(CdnImageType image_type, ConstSlice<u8> response_head,
                                    u32 decoded_body_bytes, u64 now_seconds,
                                    const CdnByteRange* range) noexcept;

} // namespace

CdnRequestResult write_static_cdn_get_request(CdnImageType image_type,
                                              CdnHost host, ConstSlice<u8> target,
                                              ConstSlice<u8> etag, Slice<u8> output,
                                              CdnByteRange range) noexcept {
    if (range.bytes && (image_type != CdnImageType::mp4 || etag.count ||
                        range.first > kMediaMaximumProgressiveVideoBytes - u64{range.bytes}))
        return CdnRequestResult{0, CdnHttpError::invalid_input};
    const Utf8View host_name = cdn_host_name(host);
    if ((!output.data && output.count != 0) || host_name.empty() || !valid_target(target) ||
        (image_type == CdnImageType::soundboard_sound &&
         (host != CdnHost::cdn || !soundboard_target_valid(target))) ||
        (image_type == CdnImageType::lottie_json &&
         (host != CdnHost::cdn || !sticker_json_target_valid(target))) ||
        ((host == CdnHost::flag_assets) != (image_type == CdnImageType::regional_flag_svg)) ||
        (host == CdnHost::activity_art && (image_type != CdnImageType::jpeg || !activity_art_target_valid(target))) ||
        (cdn_catalog_provider(host) && !catalog_provider_request_valid(host, image_type, target)) ||
        (image_type == CdnImageType::regional_flag_svg &&
         !regional_flag_svg_target_sequence({reinterpret_cast<const char*>(target.data), target.count}))) {
        return CdnRequestResult{0, CdnHttpError::invalid_target};
    }
    if (!valid_etag(etag)) {
        return CdnRequestResult{0, CdnHttpError::invalid_etag};
    }
    u8 request[kCdnMaximumRequestHeadBytes];
    ProtocolWriter writer{Slice<u8>{request, sizeof(request)}, 0};
    const bool written =
        protocol_writer_append_literal(&writer, "GET ") && protocol_writer_append_bytes(&writer, target) &&
        protocol_writer_append_literal(&writer, " HTTP/1.1\r\nHost: ") &&
        protocol_writer_append_bytes(
            &writer, ConstSlice<u8>{reinterpret_cast<const u8*>(host_name.data), host_name.bytes}) &&
        protocol_writer_append_literal(&writer, "\r\nAccept: ") &&
        (image_type == CdnImageType::png    ? protocol_writer_append_literal(&writer, "image/png")
         : image_type == CdnImageType::webp ? protocol_writer_append_literal(&writer, "image/webp")
         : image_type == CdnImageType::gif  ? protocol_writer_append_literal(&writer, "image/gif")
         : image_type == CdnImageType::jpeg ? protocol_writer_append_literal(&writer, "image/jpeg")
         : image_type == CdnImageType::ogg_opus
             ? protocol_writer_append_literal(&writer, "audio/ogg")
         : image_type == CdnImageType::mp3 ? protocol_writer_append_literal(&writer, "audio/mpeg")
         : image_type == CdnImageType::wav ? protocol_writer_append_literal(&writer, "audio/wav")
         : image_type == CdnImageType::m4a ? protocol_writer_append_literal(&writer, "audio/mp4")
         : image_type == CdnImageType::soundboard_sound
             ? protocol_writer_append_literal(&writer, "audio/mpeg, audio/ogg")
         : image_type == CdnImageType::lottie_json
             ? protocol_writer_append_literal(&writer, "application/json")
         : image_type == CdnImageType::regional_flag_svg
             ? protocol_writer_append_literal(&writer, "image/svg+xml")
             : protocol_writer_append_literal(&writer, "video/mp4")) &&
        protocol_writer_append_literal(&writer, "\r\nAccept-Encoding: identity"
                                                "\r\nConnection: keep-alive\r\n") &&
        (etag.count == 0 || (protocol_writer_append_literal(&writer, "If-None-Match: ") &&
                             protocol_writer_append_bytes(&writer, etag) &&
                             protocol_writer_append_literal(&writer, "\r\n"))) &&
        (range.bytes == 0 || (protocol_writer_append_literal(&writer, "Range: bytes=") &&
                              protocol_writer_append_u64(&writer, range.first) &&
                              protocol_writer_append_literal(&writer, "-") &&
                              protocol_writer_append_u64(&writer, range.first + range.bytes - 1) &&
                              protocol_writer_append_literal(&writer, "\r\n"))) &&
        protocol_writer_append_literal(&writer, "\r\n");
    if (!written || writer.used > output.count) {
        return CdnRequestResult{0, CdnHttpError::output_capacity};
    }
    copy_bytes(output.data, request, writer.used);
    return CdnRequestResult{writer.used, CdnHttpError::none};
}

CdnRedirectResult parse_cdn_redirect(CdnHost current_host, ConstSlice<u8> location,
                                     Slice<u8> target_output) noexcept {
    if (cdn_host_name(current_host).empty() || !location.data || location.count == 0 ||
        (!target_output.data && target_output.count != 0)) {
        return CdnRedirectResult{CdnHost::none, 0, CdnHttpError::invalid_input};
    }
    CdnHost target_host = current_host;
    ConstSlice<u8> target = location;
    if (location[0] != '/') {
        constexpr char scheme[] = "https://";
        constexpr u32 scheme_bytes = sizeof(scheme) - 1;
        if (location.count <= scheme_bytes ||
            !slice_equal_case(ConstSlice<u8>{location.data, scheme_bytes},
                              ConstSlice<u8>{reinterpret_cast<const u8*>(scheme), scheme_bytes})) {
            return CdnRedirectResult{CdnHost::none, 0, CdnHttpError::invalid_location};
        }
        u32 slash = scheme_bytes;
        while (slash < location.count && location[slash] != '/') {
            ++slash;
        }
        if (slash == location.count) {
            return CdnRedirectResult{CdnHost::none, 0, CdnHttpError::invalid_location};
        }
        const ConstSlice<u8> authority{location.data + scheme_bytes, slash - scheme_bytes};
        CdnHost parsed_host = CdnHost::none;
        if (cdn_catalog_provider(current_host)) {
            const auto name = cdn_host_name(current_host);
            if (slice_equal_case(authority, {reinterpret_cast<const u8*>(name.data), name.bytes}))
                parsed_host = current_host;
        }
        constexpr CdnHost allowed_hosts[] = {CdnHost::cdn, CdnHost::media,
                                             CdnHost::images_ext_1,
                                             CdnHost::images_ext_2};
        for (CdnHost candidate : allowed_hosts) {
            if (cdn_catalog_provider(current_host))
                break;
            const Utf8View name = cdn_host_name(candidate);
            if (slice_equal_case(authority,
                                 ConstSlice<u8>{reinterpret_cast<const u8*>(name.data), name.bytes})) {
                parsed_host = candidate;
                break;
            }
        }
        if (parsed_host == CdnHost::none) {
            return CdnRedirectResult{CdnHost::none, 0, CdnHttpError::invalid_location};
        }
        target_host = parsed_host;
        target = ConstSlice<u8>{location.data + slash, location.count - slash};
    }
    if (!valid_target(target) ||
        (cdn_catalog_provider(current_host) && !catalog_provider_target_valid(current_host, target))) {
        return CdnRedirectResult{CdnHost::none, 0, CdnHttpError::invalid_location};
    }
    if (target.count > target_output.count) {
        return CdnRedirectResult{CdnHost::none, 0, CdnHttpError::output_capacity};
    }
    copy_bytes(target_output.data, target.data, target.count);
    return CdnRedirectResult{target_host, target.count, CdnHttpError::none};
}

CdnResponseResult validate_static_image_response(CdnImageType image_type,
                                                 ConstSlice<u8> response_head,
                                                 u32 decoded_body_bytes,
                                                 u64 now_seconds) noexcept {
    return validate_response(image_type, response_head, decoded_body_bytes, now_seconds, nullptr);
}

CdnResponseResult validate_cdn_range_response(CdnImageType image_type,
                                              ConstSlice<u8> response_head,
                                              u32 decoded_body_bytes,
                                              CdnByteRange requested,
                                              u64 now_seconds) noexcept {
    if (image_type != CdnImageType::mp4 || !requested.bytes)
        return response_failure(CdnHttpError::invalid_input);
    return validate_response(image_type, response_head, decoded_body_bytes, now_seconds, &requested);
}

namespace {
CdnResponseResult validate_response(CdnImageType image_type, ConstSlice<u8> response_head,
                                    u32 decoded_body_bytes, u64 now_seconds,
                                    const CdnByteRange* range) noexcept {
    if (!response_head.data || response_head.count == 0) {
        return response_failure(CdnHttpError::invalid_input);
    }
    ResponseFields fields{};
    const HttpHeadResult parsed =
        http_parse_response_head(response_head, kCdnMaximumResponseHeaderBytes,
                                 kCdnMaximumResponseFields, response_field_visitor, &fields);
    if (!parsed.ok()) {
        if (fields.error != CdnHttpError::none) {
            return response_failure(fields.error, parsed.head.status_code);
        }
        return response_failure(parsed.state == HttpParseState::need_more
                                    ? CdnHttpError::incomplete_head
                                    : CdnHttpError::malformed_head,
                                parsed.head.status_code);
    }
    if (parsed.head.header_bytes != response_head.count) {
        return response_failure(CdnHttpError::malformed_head, parsed.head.status_code);
    }
    const HttpResponseHead& head = parsed.head;
    const u32 body_limit = cdn_encoded_body_limit(image_type);
    if (head.has_content_length && head.content_length > body_limit) {
        return response_failure(CdnHttpError::body_too_large, head.status_code);
    }
    if (decoded_body_bytes > body_limit) {
        return response_failure(CdnHttpError::body_too_large, head.status_code);
    }

    CdnResponseResult result{};
    result.etag = fields.etag;
    result.location = fields.location;
    result.header_bytes = head.header_bytes;
    result.body_bytes = decoded_body_bytes;
    result.status_code = head.status_code;
    result.store_allowed = !fields.no_store;
    result.must_revalidate = fields.no_cache || !fields.has_max_age;
    u64 freshness = fields.has_max_age ? minimum(fields.max_age, kCdnMaximumFreshnessSeconds) : 0;
    if (fields.has_age) {
        freshness = fields.age >= freshness ? 0 : freshness - fields.age;
    }
    result.expires_at_seconds = deadline_after(now_seconds, freshness);

    const bool partial = range && head.status_code == 206;
    if (head.status_code == 200 || partial) {
        // Transport MIME does not prove a codec. Audio admission still parses
        // the complete source as the exact format selected by its asset key.
        const bool normalized_audio =
            image_type == CdnImageType::mp3 || image_type == CdnImageType::wav ||
            image_type == CdnImageType::m4a || image_type == CdnImageType::soundboard_sound;
        auto content_type = fields.content_type;
        if (normalized_audio && fields.has_content_type) {
            for (u32 i = 0; i < content_type.count; ++i)
                if (content_type[i] == ';') {
                    content_type.count = i;
                    break;
                }
            content_type = http_trim_ows(content_type);
        }
        const char* expected_content_type =
            image_type == CdnImageType::png                 ? "image/png"
            : image_type == CdnImageType::ogg_opus          ? "audio/ogg"
            : image_type == CdnImageType::mp3               ? "audio/mpeg"
            : image_type == CdnImageType::wav               ? "audio/wav"
            : image_type == CdnImageType::m4a               ? "audio/mp4"
            : image_type == CdnImageType::soundboard_sound  ? "audio/mpeg"
            : image_type == CdnImageType::webp              ? "image/webp"
            : image_type == CdnImageType::gif               ? "image/gif"
            : image_type == CdnImageType::jpeg              ? "image/jpeg"
            : image_type == CdnImageType::lottie_json       ? "application/json"
            : image_type == CdnImageType::regional_flag_svg ? "image/svg+xml"
                                                                   : "video/mp4";
        const bool audio_content_type =
            (image_type == CdnImageType::ogg_opus &&
             (http_ascii_equal_case_insensitive(fields.content_type, "application/ogg") ||
              http_ascii_equal_case_insensitive(fields.content_type, "audio/ogg; codecs=opus") ||
              http_ascii_equal_case_insensitive(fields.content_type, "audio/ogg; codecs=\"opus\""))) ||
            (image_type == CdnImageType::mp3 &&
             http_ascii_equal_case_insensitive(content_type, "audio/mp3")) ||
            // Sounds are admitted by their own bytes (MP3 frames or an Ogg Opus stream).
            (image_type == CdnImageType::soundboard_sound &&
             (http_ascii_equal_case_insensitive(content_type, "audio/mp3") ||
              http_ascii_equal_case_insensitive(content_type, "audio/ogg") ||
              http_ascii_equal_case_insensitive(content_type, "audio/opus") ||
              http_ascii_equal_case_insensitive(content_type, "application/ogg"))) ||
            // QuickTime movies share the ISO base media container the MP4 demuxer reads.
            (image_type == CdnImageType::mp4 &&
             http_ascii_equal_case_insensitive(fields.content_type, "video/quicktime")) ||
            (image_type == CdnImageType::m4a &&
             (http_ascii_equal_case_insensitive(content_type, "audio/x-m4a") ||
              http_ascii_equal_case_insensitive(content_type, "audio/m4a"))) ||
            (image_type == CdnImageType::wav &&
             (http_ascii_equal_case_insensitive(content_type, "audio/x-wav") ||
              http_ascii_equal_case_insensitive(content_type, "audio/wave") ||
              http_ascii_equal_case_insensitive(content_type, "audio/vnd.wave"))) ||
            (normalized_audio &&
             (http_ascii_equal_case_insensitive(content_type, "application/octet-stream") ||
              http_ascii_equal_case_insensitive(content_type, "binary/octet-stream")));
        if (!fields.has_content_type ||
            (!http_ascii_equal_case_insensitive(content_type, expected_content_type) &&
             !audio_content_type)) {
            return response_failure(CdnHttpError::invalid_content_type, head.status_code);
        }
        if (fields.has_content_encoding &&
            !http_ascii_equal_case_insensitive(fields.content_encoding, "identity")) {
            return response_failure(CdnHttpError::invalid_content_encoding, head.status_code);
        }
        if (decoded_body_bytes == 0 ||
            (head.has_content_length && head.content_length != decoded_body_bytes)) {
            return response_failure(CdnHttpError::body_size_mismatch, head.status_code);
        }
        result.disposition = CdnResponseDisposition::accept_body;
        result.entity_bytes = decoded_body_bytes;
        if (partial) {
            u64 first = 0, last = 0, length = 0;
            if (!fields.has_content_range || !parse_content_range(fields.content_range, &first, &last, &length) ||
                first != range->first || last - first + 1 != decoded_body_bytes ||
                last - first >= range->bytes || length > kMediaMaximumProgressiveVideoBytes) {
                return response_failure(CdnHttpError::invalid_content_range, head.status_code);
            }
            result.disposition = CdnResponseDisposition::accept_partial;
            result.range_first = first;
            result.entity_bytes = length;
        }
    } else if (range && (head.status_code == 403 || head.status_code == 410)) {
        result.disposition = CdnResponseDisposition::source_expired;
    } else if (head.status_code == 304) {
        if (decoded_body_bytes != 0) {
            return response_failure(CdnHttpError::body_size_mismatch, head.status_code);
        }
        result.disposition = CdnResponseDisposition::not_modified;
    } else if (head.status_code == 301 || head.status_code == 302 || head.status_code == 307 ||
               head.status_code == 308) {
        if (!fields.has_location || fields.location.count == 0) {
            return response_failure(CdnHttpError::invalid_location, head.status_code);
        }
        result.disposition = CdnResponseDisposition::redirect;
    } else {
        return response_failure(CdnHttpError::unsupported_status, head.status_code);
    }
    result.error = CdnHttpError::none;
    return result;
}
} // namespace

} // namespace maicord
